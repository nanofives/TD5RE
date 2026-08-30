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
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_tg_real_tex.h"   /* real TD5 texture pages (level014), opt-in */
#include "td5_tg_real_tex_city.h"  /* extra city facades: SF/Tokyo/Moscow */
#include "td5_tg_real_tex_r5flora.h"  /* [R5 item 18] tall Moscow park trees */
#include "td5_tg_real_tex_r7city.h"   /* [R7 item 4] more city facade variety */
#include "td5_tg_furniture_tex.h" /* real TD5 lamp/railing/banner pages     */
#include "td5re.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <direct.h>

#define LOG_TAG "track"

/* Reserved identity. Level 90 is well clear of the shipped levels (0..40ish)
 * and of td5_trackgen.py's DEFAULT_CUSTOM_LEVEL_BASE (40) so a user's
 * hand-built tracks and the auto track can coexist. */
#define TD5_TG_LEVEL_NUM   90
/* Last custom slot, so manifest-loaded tracks keep the low slots. */
#define TD5_TG_SLOT        (TD5_CUSTOM_TRACK_SLOT_BASE + TD5_CUSTOM_TRACK_MAX - 1)

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
static int tg_surface_attr(int si);
static int tg_road_page(int si);

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

/* VARIETY is the widest block in the round by design: G1 asks for more building
 * variety AND day/night skyboxes AND start/finish banner variety AND guardrail
 * variety, and every one of those is new art rather than new geometry. */
#define TD5_TG_PAGE_R8_VARIETY (TD5_TG_PAGE_R8_BASE + 56)
#define TD5_TG_R8_VARIETY_N    24

#define TD5_TG_PAGE_R8_SHAPE  (TD5_TG_PAGE_R8_BASE + 82)
#define TD5_TG_R8_SHAPE_N     10

#define TD5_TG_PAGE_R8_BIOME  (TD5_TG_PAGE_R8_BASE + 94)
#define TD5_TG_R8_BIOME_N     14

#define TD5_TG_PAGE_COUNT     (TD5_TG_PAGE_R8_BASE + 110)

#define TD5_TG_MAX_VERTICES   64000
#define TD5_TG_MAX_SPANS      3000

/* Minimum turn radius as a multiple of the road's half-width. Mirrors
 * td5_trackgen.py's CURVE_SAFETY_DEFAULT (1.5); the extra 1.2 is headroom so
 * the resampled centerline never lands exactly on the floor. */
#define TD5_TG_CURVE_SAFETY   (1.5 * 1.2)
/* Steepest allowed |dY/d(arc)|, mirroring td5_trackgen.py's max_grade.
 * Was briefly cut to 0.035 on an inverted reading of the airborne mask (see
 * the row-order note in tg_emit_strip); restored to the Python tool's value. */
#define TD5_TG_MAX_GRADE      0.12

#define TD5_TG_PI 3.14159265358979323846

/* ---------------------------------------------------------------- RNG ----- */
/* Private xorshift32 -- deliberately NOT rand(). The game's rand() is the
 * MSVC-compatible one used for sim determinism and netplay lockstep
 * (td5_msvc_rand.c); drawing track geometry from it would perturb every
 * downstream random draw and break trace goldens. */
static unsigned int s_rng;

/* Seed to run the S2 regen gate against, set during a build and consumed after
 * it finishes (the gate itself rebuilds a centerline, so it cannot run from
 * inside one). 0 = nothing to check. */
static unsigned int s_selfcheck_regen_seed = 0;

/* Branch fork descriptors, produced by tg_build_strip and consumed by the header
 * jump-table write and by tg_emit_models (multi-fork). Each fork splits the ring
 * at span F into a main half [F+1..F+len] and a corridor appended after the ring
 * at [cbase..cbase+len-1], rejoining at span R. The loader (td5_track.c) already
 * iterates an N-entry jump table, so the whole chain is multi-fork. */
#define TD5_TG_BRANCH_MAX 4
/* sep = per-fork separation scale in [0,1] (item 10): how far the branch bows
 * away from the main carriageway, as a fraction of the widest bow tg_branch_bow
 * allows. Small = a divided AVENUE (the two carriageways stay close, split only
 * by a central median); large = a road that genuinely diverges in two. Stored
 * on the fork so the strip rows, the road mesh, the clearance query and the
 * divider all read ONE value and cannot drift. */
typedef struct { int F, len, cbase, R; double sep; } TG_Fork;
static TG_Fork s_forks[TD5_TG_BRANCH_MAX];
static int s_fork_count;
static int s_ring_len;

/* Fork lookups live down in the [FB] block (three other work areas read them
 * from there); forward-declared here because the strip emitters above need
 * them to map an APPENDED corridor span back to its main-ring node. */
static int tg_fork_of_main(int si);
static int tg_fork_of_corridor(int si, int *k);
static int tg_city_crossing_here(int si);   /* [R3 item 7] fence break gate */
/* [R4 CROSS item 4] a zebra must not mark a PARK frontage, so tg_city_crossing_here
 * (defined above the park block) needs the park predicate forward-declared. */
static int tg_block_is_park(int si, int left);
/* Same reason: the finish-line placer below has to keep the finish (and its
 * gantry) out of a tunnel run, and the tunnel test is defined with the tunnel
 * emitters much further down. */
static int tg_span_in_tunnel(int si);
/* Per-biome bridge/tunnel weighting. The two run gates sit up here (the strip
 * pass needs them) but the biome table is defined with the biome block much
 * further down, so the accessors are forward-declared the same way. */
static int tg_biome_bridge_pct(int si);
static int tg_biome_tunnel_pct(int si);
/* [R3 item 15] the centreline walk (above the bridge block) clamps curvature on
 * bridge spans, so it needs the run gate before that gate is defined. */
static int tg_span_in_bridge_run(int si);
/* Largest heading change per span allowed on a bridge deck, radians. 0.03 =>
 * turn radius >= ~50000 units, a sweeping curve rather than a hairpin, so a
 * deliberate bridge that lands on a tight section is straightened out. */
#define TD5_TG_BRIDGE_MAX_TURN 0.03
/* [R6 item 10] the centreline walk clamps curvature on FORK spans too -- a fork
 * on a sharp bend folds its shifted main half-carriageway and its bowed branch
 * corridor together, and a car driving the region is lifted (the "span 570
 * collision is LIFTING the car" report + the paired "avoid this kind of curve"
 * request). tg_span_in_fork_run gates it; forward-declared like the bridge one. */
static int tg_span_in_fork_run(int si);
/* Largest heading change per span allowed across a fork's span range, radians.
 * 0.045 => turn radius >= ~33000 units, a sweeping curve. Sharper than this and
 * the quarter-road main shift + full-width branch bow overlap on the inside of
 * the bend. Looser than the bridge cap (a fork is a longer feature and a dead-
 * straight 120-span corridor reads worse than a gentle sweep) but still well
 * inside the fold threshold (measured folds began around 0.10 rad/span). */
#define TD5_TG_FORK_MAX_TURN 0.045
/* [R3 BLOCK] item 4: the centerline walk (above the biome block) needs to know
 * whether span si is a CITY cell to place "around the block" turns there. */
static int tg_biome_span_is_city(int si);

/* [R5 item 15] True where a street-wall facade actually STANDS at span si (not
 * merely where the run/gap pattern says "built"). Forward-declared: it reads the
 * biome table and bridge runs, both defined below, but tg_side_built above needs
 * it. */
static int tg_facade_stands(int si);

/* Seed of the last successful build, for reproducing a good random track. */
static unsigned int s_last_seed = 0;

static void tg_srand(unsigned int seed)
{
    s_rng = seed ? seed : 0x9E3779B9u;
}

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

    TG_ACCT_R8_TERRAIN,     /* TERRAIN(99991 5; 777 14,15,16)  rename in place */

    TG_ACCT_R8_VARIETY,     /* VARIETY(G1; 99991 6)         rename in place */

    TG_ACCT_R8_SHAPE,       /* SHAPE  (G2,G3,G5,G4-part)    rename in place */

    TG_ACCT_R8_BIOME,       /* BIOME  (777 19; G4-part)     rename in place */
    TG_ACCT_KIND_COUNT
} TG_AcctKind;

static const char *const k_acct_names[TG_ACCT_KIND_COUNT] = {
    "buildings", "sidewalks", "shopfronts", "fences",   "lamps",
    "crossings", "trees",     "props",      "water",    "bridge-pieces",
    "tunnel-pieces", "terrain", "far-bands", "banners", "guardrails",
    "road-quads", "checkpoints", "branch-nodes",
    /* [R3] appended in enum order: BLOCK's park/house, CITY's step wall. */
    "parks", "houses", "step-walls",
    /* [R4] one reserved name per area, in enum order, each on its own line so
     * two areas renaming their own slot touch non-adjacent lines. Rename the
     * string to match your renamed enum constant. Keep the trailing comma on
     * every line but the last -- the R3 union merge lost exactly one comma and
     * two names silently concatenated into a single string literal. */
    "r4-flow",              /* FLOW   */
    "cross-furn",           /* CROSS  */
    "fork-back",            /* CITY   */
    "r4-branch",            /* BRANCH */
    "coastline",            /* BRIDGE item 20 */
    /* [R5] one reserved name per area, in enum order, each on its own line and
     * spaced by a blank line. Rename the string to match your renamed enum
     * constant. Keep the trailing comma on every line but the last. */
    "r5-cross",             /* CROSS  */

    "r5-city",              /* CITY   */

    "bridge-kerb",          /* BRIDGE (item 17 deck kerb) */

    "branch-verge",         /* STRUCT item 10 */

    "r5-flora",             /* FLORA  */
    /* [R6] one reserved name per area, in enum order, blank-line spaced.
     * Rename to match your renamed enum constant; keep every comma. */
    "r6-cross",             /* CROSS  */

    "r6-city",              /* CITY   */

    "r6-branch",            /* BRANCH */

    "r6-tunnel",            /* TUNNEL */

    "r6-bridge",            /* BRIDGE */

    "r6-flora",             /* FLORA  */
    /* [R7] one reserved name per area, in enum order, blank-line spaced.
     * Rename to match your renamed enum constant; keep every comma. */
    "guard-rejects",        /* GUARD  */

    "r7-cross",             /* CROSS  */

    "r7-city",              /* CITY   */

    "r7-branch",            /* BRANCH */

    "r7-bridge",            /* BRIDGE */

    "r7-flora",             /* FLORA  */
    /* [R8] one reserved name per area, in enum order, blank-line spaced.
     * Rename to match your renamed enum constant; keep every comma. Only the
     * LAST entry omits its trailing comma -- the R3 union merge lost exactly
     * one comma here and two names concatenated into a single literal. */
    "r8-guard",             /* GUARD   */

    "r8-cross",             /* CROSS   */

    "r8-city",              /* CITY    */

    "r8-bridge",            /* BRIDGE  */

    "r8-terrain",           /* TERRAIN */

    "r8-variety",           /* VARIETY */

    "r8-shape",             /* SHAPE   */

    "r8-biome"              /* BIOME   */
};

static long s_acct_count[TG_ACCT_KIND_COUNT];
/* Presence bitmap: bit k of s_acct_mask[si] = kind k stands at span si. One
 * word per span costs 24 KB at TD5_TG_MAX_SPANS and buys exact run extraction,
 * which a running min/max pair cannot give (a kind present at spans 0-40 and
 * 900-940 would otherwise report as one run 0..940).
 * WIDENED to 64 bits for round 4: the kind count reached 26 of the 32 bits an
 * unsigned int offers, and overflowing it would not fail loudly -- it would
 * silently shift a bit out and report a real emitter as "NONE emitted", which
 * is the one lie this inventory exists to prevent. */
static unsigned long long s_acct_mask[TD5_TG_MAX_SPANS];

static void tg_acct_reset(void)
{
    memset(s_acct_count, 0, sizeof(s_acct_count));
    memset(s_acct_mask,  0, sizeof(s_acct_mask));
}

/* n elements of `kind` standing at span si. si out of range still COUNTS (the
 * total must stay truthful) but contributes no run -- an emitter placing
 * something off the ring is itself a finding, and silently dropping it would
 * hide it. */
static void tg_acct_n(TG_AcctKind kind, int si, int n)
{
    if ((unsigned)kind >= TG_ACCT_KIND_COUNT || n <= 0) return;
    s_acct_count[kind] += n;
    if (si >= 0 && si < TD5_TG_MAX_SPANS)
        s_acct_mask[si] |= 1ull << (unsigned)kind;
}

static void tg_acct(TG_AcctKind kind, int si) { tg_acct_n(kind, si, 1); }

/* One element that OCCUPIES spans si0..si1 (a bridge deck, a tunnel bore, a
 * water plane). Counted once -- it is one object -- but marked present across
 * its whole extent so the run list brackets every span you can see it from. */
static void tg_acct_range(TG_AcctKind kind, int si0, int si1)
{
    int s;
    if ((unsigned)kind >= TG_ACCT_KIND_COUNT) return;
    if (si1 < si0) { s = si0; si0 = si1; si1 = s; }
    s_acct_count[kind] += 1;
    if (si0 < 0) si0 = 0;
    if (si1 >= TD5_TG_MAX_SPANS) si1 = TD5_TG_MAX_SPANS - 1;
    for (s = si0; s <= si1; s++)
        s_acct_mask[s] |= 1ull << (unsigned)kind;
}

/* Cap on run entries printed per kind. A kind with more runs than this is
 * scattered rather than placed, and its exact run list is not what anyone reads
 * the log for -- the count and the first/last span are. */
#define TD5_TG_ACCT_MAX_RUNS 10

static void tg_acct_report(int nspans)
{
    int k;
    if (nspans > TD5_TG_MAX_SPANS) nspans = TD5_TG_MAX_SPANS;
    TD5_LOG_I(LOG_TAG, "trackgen: ---- element inventory (%d spans) ----", nspans);
    for (k = 0; k < TG_ACCT_KIND_COUNT; k++) {
        const unsigned long long bit = 1ull << (unsigned)k;
        char runs[240];
        int  pos = 0, nruns = 0, touched = 0, first = -1, last = -1;
        int  s = 0;

        if (s_acct_count[k] == 0) continue;
        runs[0] = '\0';
        while (s < nspans) {
            int a;
            if (!(s_acct_mask[s] & bit)) { s++; continue; }
            a = s;
            while (s < nspans && (s_acct_mask[s] & bit)) s++;
            touched += s - a;
            if (first < 0) first = a;
            last = s - 1;
            nruns++;
            if (nruns <= TD5_TG_ACCT_MAX_RUNS && pos < (int)sizeof(runs) - 24) {
                pos += snprintf(runs + pos, sizeof(runs) - (size_t)pos,
                                "%s%d-%d", pos ? "," : "", a, s - 1);
            }
        }
        if (nruns > TD5_TG_ACCT_MAX_RUNS)
            snprintf(runs + pos, sizeof(runs) - (size_t)pos,
                     ",+%d more", nruns - TD5_TG_ACCT_MAX_RUNS);
        TD5_LOG_I(LOG_TAG,
                  "trackgen:   %-14s n=%-6ld spans=%-5d first=%-5d last=%-5d runs[%d]: %s",
                  k_acct_names[k], s_acct_count[k], touched, first, last,
                  nruns, runs[0] ? runs : "-");
    }
    /* Empty kinds are reported as a single line rather than skipped silently:
     * "trees n=0" is a finding, and a reader who does not see the kind at all
     * cannot tell "none emitted" from "not accounted yet". */
    {
        char none[320];
        int pos = 0;
        for (k = 0; k < TG_ACCT_KIND_COUNT; k++) {
            if (s_acct_count[k] != 0) continue;
            if (pos < (int)sizeof(none) - 20)
                pos += snprintf(none + pos, sizeof(none) - (size_t)pos,
                                "%s%s", pos ? " " : "", k_acct_names[k]);
        }
        if (pos)
            TD5_LOG_I(LOG_TAG, "trackgen:   NONE emitted: %s", none);
    }
    TD5_LOG_I(LOG_TAG, "trackgen: ---- end inventory ----");
}

/* ==========================================================================
 * TIME OF DAY  (feedback R2 item 22)
 *
 * Night is a property of the RACE, not of a span, a biome or a texture page, so
 * it is decided ONCE when the race is entered (td5_trackgen_regenerate, which is
 * what a race launch calls) and latched. Emitters that need it -- street lamps
 * only lighting up at night, headlights, sky choice -- read the latch through
 * td5_trackgen_is_night() instead of each rolling its own predicate, which is
 * how a track ends up with lit lamps under a noon sky.
 *
 * Latching also makes it stable across the build: tg_emit_models runs thousands
 * of times per track and a predicate that re-rolled per call would light every
 * other lamp.
 *
 * TD5RE_AUTOTRACK_NIGHT: 0 = always day, 1 = always night, 2 = decide from the
 * seed (default). Seed-derived keeps a given seed reproducible -- the same seed
 * is the same track at the same time of day, which the whole generator relies on.
 * ========================================================================== */
static int s_is_night = 0;

static void tg_decide_night(unsigned int seed)
{
    int mode = td5_env_int("TD5RE_AUTOTRACK_NIGHT", 2, 0, 2);
    if (mode < 2) {
        s_is_night = mode;
    } else {
        /* Knuth multiplicative hash of the seed, high bit. ~1 in 4 night, which
         * is roughly the shipped TD5 ratio (5 of the 19 schedule tracks run at
         * night or dusk) rather than a coin flip. */
        unsigned int h = seed * 2654435761u;
        s_is_night = ((h >> 29) == 0) ? 1 : 0;
    }
    TD5_LOG_I(LOG_TAG, "trackgen: time of day = %s (seed=%u knob=%d)",
              s_is_night ? "NIGHT" : "DAY", seed, mode);
}

int td5_trackgen_is_night(void) { return s_is_night; }

static unsigned int tg_rand(void)
{
    unsigned int x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

/* Uniform in [lo, hi] inclusive. */
static int tg_range(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(tg_rand() % (unsigned int)(hi - lo + 1));
}

/* Uniform in [0, 1). */
static double tg_frand(void)
{
    return (double)(tg_rand() >> 8) / 16777216.0;
}

/* ------------------------------------------------------- centerline ------- */
typedef struct {
    double x, y, z;      /* world units */
    double width;        /* full road width, world units */
    int    lanes;        /* 1..12 */
    double tx, tz;       /* unit tangent (filled after the walk) */
} TG_Node;

typedef struct {
    TG_Node *v;
    int      count;
    int      cap;
} TG_NodeList;

static int tg_nodes_reserve(TG_NodeList *nl, int need)
{
    if (need <= nl->cap) return 1;
    {
        int cap = nl->cap ? nl->cap : 256;
        TG_Node *nv;
        while (cap < need) cap *= 2;
        nv = (TG_Node *)realloc(nl->v, (size_t)cap * sizeof(TG_Node));
        if (!nv) return 0;
        nl->v = nv;
        nl->cap = cap;
    }
    return 1;
}

static int tg_nodes_push(TG_NodeList *nl, double x, double z,
                         double width, int lanes)
{
    if (!tg_nodes_reserve(nl, nl->count + 1)) return 0;
    {
        TG_Node *n = &nl->v[nl->count++];
        n->x = x; n->y = 0.0; n->z = z;
        n->width = width;
        n->lanes = lanes;
        n->tx = 0.0; n->tz = 1.0;
    }
    return 1;
}

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

static int tg_too_close(const TG_NodeList *nl, double x, double z,
                        double width, double lane_width, int skip)
{
    const int limit = nl->count - skip;
    int i;
    for (i = 0; i < limit; i++) {
        double dx = nl->v[i].x - x;
        double dz = nl->v[i].z - z;
        double need = (nl->v[i].width + width) * 0.5 + lane_width * 0.25;
        if (dx * dx + dz * dz < need * need) return 1;
    }
    return 0;
}

/* Acute heading budget in radians, from the env knob, clamped so cos(limit)>0
 * (the non-trapping proof requires strictly-positive forward progress). */
static double tg_acute_heading_limit(void)
{
    int deg = td5_env_int("TD5RE_AUTOTRACK_ACUTE_DEG", TD5_TG_ACUTE_HEADING_DEG,
                          TD5_TG_ACUTE_HEADING_MIN, TD5_TG_ACUTE_HEADING_MAX);
    return (double)deg * TD5_TG_PI / 180.0;
}

/* Derived adjacent-skip window (see the self-intersection guard note): the
 * smallest N such that two nodes N spans apart along the road provably cannot
 * be within tg_too_close's "need" distance of each other.
 *
 * ROOT CAUSE of "geometry can overlap on acute curves" (2026-08-27). This used
 * to derive N from the AXIS advance -- dmin = span_len * cos(limit_max), the
 * guaranteed progress along TD5_TG_AXIS_HEADING. That is the right quantity for
 * the NON-TRAPPING proof and the wrong one for SEPARATION, because cos(88 deg)
 * is 0.035: at the default acute budget it produced N = 351. Every pair of
 * nodes less than 351 spans apart was therefore EXEMPT from the overlap test,
 * and an acute section is only 4..12 spans long -- a run of them can double the
 * road back alongside itself well inside that window with nothing checking it.
 * The road then interpenetrates, which is exactly the reported symptom (and the
 * span walker snaps to the wrong span there, the same failure the guard exists
 * to prevent).
 *
 * Separation does not come from the heading budget at all. It comes from the
 * CURVATURE SAFETY floor: every turning span is clamped to
 * radius >= (width/2) * curve_safety, so the WORST case for two nodes n spans
 * apart is a single arc at exactly that radius, where their separation is the
 * chord c(n) = 2r * sin(n * span_len / (2r)). Anything less curved -- a
 * straight, a sweeping curve, an S, a hairpin made of two arcs -- puts them
 * further apart, not closer. So N is the smallest n with c(n) >= need_max:
 *
 *     n >= (2r / span_len) * asin(need_max / (2r)),   2r = curve_safety * w_max
 *
 * At 12 lanes and curve_safety 1.8 that is ~13 spans rather than 351, so the
 * 13..351 band -- where the real overlaps live -- is now CHECKED instead of
 * assumed away. 2r > need_max holds for any road at least one lane wide, so the
 * asin argument stays in range; the degenerate branch below covers the rest.
 *
 * Knobs: TD5RE_AUTOTRACK_ADJ_SKIP (0 = derive, else force a window) and
 * TD5RE_AUTOTRACK_SKIP_AXIS=1 (restore the old axis-derived window), both for
 * bisecting a track that comes out shorter than wanted -- a tighter window
 * rejects more sections, so the walk falls back to straights more often. */
static int tg_adjacent_skip(const TD5_TrackGenSpec *spec, double limit_max)
{
    const double span_len   = (double)spec->span_length;
    const double lane_width = (double)spec->lane_width;
    const double w_max      = (double)TD5_TG_MAX_LANES * lane_width;
    /* Worst-case need in tg_too_close: both roads at the max width. */
    const double need_max   = w_max + lane_width * 0.25;
    const double safety     = (spec->curve_safety_x100 > 0)
                            ? (double)spec->curve_safety_x100 / 100.0
                            : TD5_TG_CURVE_SAFETY;
    const double two_r      = safety * w_max;    /* 2x the tightest legal radius */
    const int    forced     = td5_env_int("TD5RE_AUTOTRACK_ADJ_SKIP", 0, 0, 4000);
    const double step       = (span_len > 1.0) ? span_len : 1.0;
    int skip;

    if (forced > 0) return forced;

    if (td5_env_flag_off("TD5RE_AUTOTRACK_SKIP_AXIS")) {
        double dmin = span_len * cos(limit_max);
        if (dmin < 1.0) dmin = 1.0;              /* guard against cos -> 0 */
        skip = (int)ceil(need_max / dmin);
    } else if (two_r > need_max) {
        /* +2 spans of margin: the width RAMPS across a dual-lane taper, so the
         * two nodes need not carry the same width and the closed form above is
         * a bound rather than an identity. */
        skip = (int)ceil((two_r / step) * asin(need_max / two_r)) + 2;
    } else {
        /* Degenerate spec (a road as wide as its own tightest turn): fall back
         * to the straight-line bound, which is always safe. */
        skip = (int)ceil(need_max / step) + 2;
    }
    if (skip < 1) skip = 1;
    return skip;
}

/* Pick a section type from the normalised weights. */
static TD5_TrackGenSection tg_pick_section(const TD5_TrackGenSpec *spec)
{
    int total = 0, i, roll;
    for (i = 0; i < TD5_TG_SECTION_COUNT; i++) {
        int w = spec->weight[i];
        if (w > 0) total += w;
    }
    if (total <= 0) return TD5_TG_STRAIGHT;
    roll = tg_range(0, total - 1);
    for (i = 0; i < TD5_TG_SECTION_COUNT; i++) {
        int w = spec->weight[i];
        if (w <= 0) continue;
        if (roll < w) return (TD5_TrackGenSection)i;
        roll -= w;
    }
    return TD5_TG_STRAIGHT;
}

static const char *tg_section_name(TD5_TrackGenSection s)
{
    switch (s) {
        case TD5_TG_STRAIGHT:  return "straight";
        case TD5_TG_CURVE:     return "curve";
        case TD5_TG_ACUTE:     return "acute";
        case TD5_TG_DUAL_LANE: return "dual-lane";
        default:               return "?";
    }
}

/* Walk the section picker until target_spans spans' worth of nodes exist.
 * Node i and i+1 bracket span i, so we need target_spans+1 nodes. */
static int tg_build_centerline(const TD5_TrackGenSpec *spec, TG_NodeList *nl,
                              int section_tally[TD5_TG_SECTION_COUNT])
{
    const int    want_nodes = spec->target_spans + 1;
    const double span_len   = (double)spec->span_length;
    const double base_width = (double)spec->lanes * (double)spec->lane_width;

    double x = 0.0, z = 0.0;
    /* Absolute heading; 0 = +Z. Wanders within +/-TD5_TG_HEADING_LIMIT of
     * TD5_TG_AXIS_HEADING, which keeps the walk non-trapping AND keeps the
     * route heading byte clear of the junction sentinel. */
    double heading = TD5_TG_AXIS_HEADING;
    double width = base_width;        /* current, ramped toward target_width */
    /* Max width change per span so widenings taper instead of stepping. */
    const double width_ramp = (double)spec->lane_width * 0.5;

    /* Heading budgets and the adjacent-skip derived from them. ACUTE sections
     * get a sharper budget (see TD5_TG_ACUTE_HEADING_DEG); the derived skip is
     * sized off the LARGER budget so the non-trapping / no-self-intersection
     * guarantee holds across all section types. */
    const double acute_limit = tg_acute_heading_limit();
    const double limit_max    = (acute_limit > TD5_TG_HEADING_LIMIT)
                              ? acute_limit : TD5_TG_HEADING_LIMIT;
    const int    skip         = tg_adjacent_skip(spec, limit_max);
    /* Round, do not truncate. The spine limit is the literal 1.396 rad =
     * 79.985 deg, and an 88 deg acute budget round-trips through radians to
     * 87.999..., so %.0f printed "spine=79deg acute=87deg" -- which reads as
     * the knob having failed to apply, when the derived skip proves it did. */
    TD5_LOG_I(LOG_TAG, "trackgen: heading budget spine=%ddeg acute=%ddeg "
              "-> adjacent_skip=%d (curve-safety %d/100; the old heading-derived "
              "window was %d)",
              (int)(TD5_TG_HEADING_LIMIT * 180.0 / TD5_TG_PI + 0.5),
              (int)(acute_limit * 180.0 / TD5_TG_PI + 0.5), skip,
              spec->curve_safety_x100,
              (int)ceil(((double)TD5_TG_MAX_LANES * spec->lane_width
                         + spec->lane_width * 0.25)
                        / (span_len * cos(limit_max) > 1.0
                           ? span_len * cos(limit_max) : 1.0)));

    if (!tg_nodes_push(nl, x, z, width, spec->lanes)) return 0;

    /* Lead-in: straight road covering the whole starting grid plus the run to
     * the first corner. Must be at least TD5_TG_GRID_SPAN + the grid's own
     * stagger depth, since the grid places cars BEHIND the start line. */
    {
        int i;
        for (i = 0; i < TD5_TG_GRID_SPAN + 16 && nl->count < want_nodes; i++) {
            x += sin(heading) * span_len;
            z += cos(heading) * span_len;
            if (!tg_nodes_push(nl, x, z, width, spec->lanes)) return 0;
        }
    }

    /* Section attempts before we give up and end the track short. A section is
     * retried (with fresh random parameters) when it would overlap earlier
     * road; late attempts are forced straight, which is the most likely shape
     * to escape a cul-de-sac the walk has painted itself into. */
    int attempts = 0;

    while (nl->count < want_nodes) {
        TD5_TrackGenSection sec = tg_pick_section(spec);
        int    len_spans;
        double target_width = base_width;
        double radius = 0.0;
        int    dir = (tg_rand() & 1) ? 1 : -1;
        /* Rollback point, so a rejected section leaves no trace. */
        const int    save_count   = nl->count;
        const double save_x       = x;
        const double save_z       = z;
        const double save_heading = heading;
        const double save_width   = width;
        int rejected = 0;

        /* [R3 BLOCK] item 4: "around the block" 90-degree turns. In a CITY biome
         * (which the seed lays out before this walk), occasionally force a tight
         * ACUTE corner so the route reads as turning a block rather than sweeping
         * through it. The existing ACUTE machinery keeps the curvature-safety
         * floor and the non-trapping heading budget, so this reuses that guard
         * rather than adding a new turn kind. Keyed on a per-position hash (not
         * the shared RNG) so toggling the knob does not otherwise move the road.
         *
         * DEFAULT OFF (td5_env_flag_off): a shape change can leave the walk
         * boxed in and the track short -- the r2-branch item 20 lesson -- so it
         * is opt-in until driven and checked for "boxed in" in race.log. The
         * attempts guard keeps the forced-straight escape below able to win. */
        if (attempts < 8 && td5_env_flag_off("TD5RE_AUTOTRACK_BLOCK_TURNS") &&
            tg_biome_span_is_city(nl->count)) {
            const unsigned int bh = (unsigned)nl->count * 2654435761u;
            if ((bh >> 28) == 0u) sec = TD5_TG_ACUTE;   /* ~1 city section in 16 */
        }

        if (attempts >= 12) sec = TD5_TG_STRAIGHT;   /* try to escape */

        switch (sec) {
            case TD5_TG_STRAIGHT:
                len_spans = tg_range(6, 24);
                break;

            case TD5_TG_CURVE:
                len_spans = tg_range(8, 26);
                /* Sweeping: comfortably above the safety floor. */
                radius = 12000.0 + tg_frand() * 28000.0;
                break;

            case TD5_TG_ACUTE:
                len_spans = tg_range(4, 12);
                /* Tight: sit just above the curvature-safety floor for the
                 * CURRENT width, so a hairpin never self-intersects the road
                 * surface. */
                radius = (width * 0.5) * (spec->curve_safety_x100 / 100.0)
                       * (1.0 + tg_frand() * 0.6);
                break;

            case TD5_TG_DUAL_LANE:
            default: {
                int extra = tg_range(2, 4);   /* +2..+4 lanes */
                int lanes = spec->lanes + extra;
                if (lanes > 12) lanes = 12;
                target_width = (double)lanes * (double)spec->lane_width;
                len_spans = tg_range(10, 28);
                /* Gentle drift so a wide stretch is not a dead-straight slab. */
                if (tg_rand() & 1)
                    radius = 26000.0 + tg_frand() * 30000.0;
                break;
            }
        }

        /* ACUTE sections may swing to the sharper budget; everything else
         * stays on the spine budget. The forced-straight escape above sets
         * sec = STRAIGHT first, so it correctly uses the spine budget here. */
        const double heading_limit = (sec == TD5_TG_ACUTE)
                                   ? acute_limit : TD5_TG_HEADING_LIMIT;

        {
            int i;
            for (i = 0; i < len_spans && nl->count < want_nodes; i++) {
                int lanes_here;

                /* Ramp width toward this section's target. */
                if (width < target_width) {
                    width += width_ramp;
                    if (width > target_width) width = target_width;
                } else if (width > target_width) {
                    width -= width_ramp;
                    if (width < target_width) width = target_width;
                }

                /* Curvature safety is width-dependent, so re-check every span:
                 * a bend that was legal at 4 lanes can be illegal once a
                 * dual-lane taper has widened the road under it. */
                if (radius > 0.0) {
                    double floor_r = (width * 0.5) * (spec->curve_safety_x100 / 100.0);
                    double r = radius < floor_r ? floor_r : radius;
                    double dh = (double)dir * (span_len / r);
                    /* [R3 item 15] no sharp turns on a bridge. A deliberate
                     * bridge run may land on any section, including a hairpin
                     * ACUTE, and a sharply curved deck reads wrong (and would
                     * shear the transverse deck seam of item 11). Cap the
                     * per-span heading change to a sweeping minimum on bridge
                     * spans. This only ever REDUCES curvature, and a straighter
                     * road cannot self-overlap where the curved one did not, so
                     * it needs no extra rejection handling. Keyed on the node
                     * about to be pushed (index nl->count); an off-by-one at a
                     * run boundary is immaterial across a 40-span run. */
                    if (tg_span_in_bridge_run(nl->count)) {
                        if (dh >  TD5_TG_BRIDGE_MAX_TURN) dh =  TD5_TG_BRIDGE_MAX_TURN;
                        if (dh < -TD5_TG_BRIDGE_MAX_TURN) dh = -TD5_TG_BRIDGE_MAX_TURN;
                    }
                    /* [R6 item 10] Same treatment for fork spans: a fork narrows
                     * the road to its left half and shifts it a quarter-road to
                     * the inside while the branch bows a full road-width out; on a
                     * sharp bend those fold together and lift a car ("span 570").
                     * Cap the per-span heading change so every fork sits on a
                     * sweeping curve, never a hairpin -- the user's "avoid this
                     * kind of curve". Only ever REDUCES curvature (no self-overlap
                     * a straighter road did not already have). */
                    if (tg_span_in_fork_run(nl->count)) {
                        if (dh >  TD5_TG_FORK_MAX_TURN) dh =  TD5_TG_FORK_MAX_TURN;
                        if (dh < -TD5_TG_FORK_MAX_TURN) dh = -TD5_TG_FORK_MAX_TURN;
                    }
                    heading += dh;
                    /* Keep the walk non-trapping (see TD5_TG_HEADING_LIMIT).
                     * On hitting the limit, reverse the turn so the road peels
                     * back off the boundary instead of grinding along it. */
                    if (heading > TD5_TG_AXIS_HEADING + heading_limit) {
                        heading = TD5_TG_AXIS_HEADING + heading_limit;
                        dir = -1;
                    } else if (heading < TD5_TG_AXIS_HEADING - heading_limit) {
                        heading = TD5_TG_AXIS_HEADING - heading_limit;
                        dir = 1;
                    }
                }

                x += sin(heading) * span_len;
                z += cos(heading) * span_len;

                /* Lane COUNT is constant for the whole track; only the WIDTH
                 * varies. Two reasons: (a) consecutive spans must SHARE a
                 * vertex row (see tg_emit_strip) and a shared row has one
                 * point count; (b) shipped tracks only ever use 2-4 lanes, so
                 * the rail LUTs and suspension paths are only exercised there.
                 * A "dual-lane" section is therefore a visibly WIDER road, not
                 * a different subdivision -- which is the visual intent anyway,
                 * since the lane count is really a surface-grid stride. */
                lanes_here = spec->lanes;

                /* Would this node put road on top of earlier road? */
                if (tg_too_close(nl, x, z, width, (double)spec->lane_width,
                                 skip)) {
                    rejected = 1;
                    break;
                }

                if (!tg_nodes_push(nl, x, z, width, lanes_here)) return 0;
            }
        }

        if (rejected) {
            /* Roll the whole section back and try a different one. */
            nl->count = save_count;
            x         = save_x;
            z         = save_z;
            heading   = save_heading;
            width     = save_width;
            attempts++;
            if (attempts >= 24) {
                TD5_LOG_W(LOG_TAG, "trackgen: boxed in after %d spans; ending "
                          "track early (no non-overlapping continuation found)",
                          nl->count - 1);
                break;
            }
            continue;
        }

        attempts = 0;
        section_tally[sec]++;
    }

    /* Unit tangents by central difference (endpoints one-sided). */
    {
        int i;
        for (i = 0; i < nl->count; i++) {
            int a = (i > 0) ? i - 1 : i;
            int b = (i < nl->count - 1) ? i + 1 : i;
            double dx = nl->v[b].x - nl->v[a].x;
            double dz = nl->v[b].z - nl->v[a].z;
            double len = sqrt(dx * dx + dz * dz);
            if (len < 1e-6) { dx = 0.0; dz = 1.0; len = 1.0; }
            nl->v[i].tx = dx / len;
            nl->v[i].tz = dz / len;
        }
    }
    return 1;
}

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
#define TD5_TG_BRIDGE_RUN     40       /* spans per deliberate bridge (item 13) */
#define TD5_TG_BRIDGE_HEIGHT  2000.0   /* crown lift; bounded by max_grade */
#define TD5_TG_BRIDGE_CHASM   2500.0   /* how far the ground/river drops below */
/* Half-width of the river channel. Unlike the sea (which starts outboard of the
 * road edge) this crosses the CENTRELINE, which is why it is the water plane
 * that can end up over a bore -- see tg_water_span_clear. */
#define TD5_TG_BRIDGE_WATER_HALF 32000.0

static int tg_bridges_enabled(void)
{
    /* Default ON (2026-08-26); set TD5RE_AUTOTRACK_BRIDGES=0 to disable. This
     * moves the STRIP (the elevation hump), so it can affect climb/AI pacing/
     * crest jumps -- pending a drive test. */
    return td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGES");
}

/* Is span si inside a deliberately-placed bridge run? Stateless and derived
 * only from si, so the generator, the emitter and the guardrail gate all agree
 * without passing anything around. */
static int tg_span_in_bridge_run(int si)
{
    unsigned int h, thresh;
    if (!tg_bridges_enabled()) return 0;
    if (si <= TD5_TG_GRID_SPAN + 40) return 0;   /* not right off the grid */
    h = (unsigned)(si / TD5_TG_BRIDGE_RUN) * 2654435761u;
    /* Base rate ~1 run in 8, scaled by the biome's weighting (feedback: "if it
     * is a countryside there should be more bridges"). 125/1000 IS that 1-in-8,
     * so a biome at 100 keeps exactly the old rate; FIELDS at 190 gets ~24%,
     * CITY at 40 gets 5%. Keyed on the run, not the span, so a run never
     * half-exists. */
    thresh = (125u * (unsigned)tg_biome_bridge_pct(si)) / 100u;
    if (thresh > 1000u) thresh = 1000u;
    return ((h >> 8) % 1000u) < thresh;
}

/* [R6 CROSS item 15] Is span si within `clear` spans of any bridge run? A side
 * street opening right off a bridge end reads as detached from its surroundings
 * ("avoid adding crossing streets right after a bridge"), so the crossing
 * emitters use this to keep a clearance band around every run -- an explicit
 * gate in the same shape as the R4 bridge/tunnel interlock, not a rate tweak. */
#define TD5_TG_XBRIDGE_CLEAR 5
static int tg_span_near_bridge(int si, int clear)
{
    int k;
    for (k = -clear; k <= clear; k++)
        if (si + k >= 0 && tg_span_in_bridge_run(si + k)) return 1;
    return 0;
}

/* Lowest road node on the WHOLE track, cached.
 *
 * A global fact with one consumer that genuinely needs a global one: the
 * background band (tg_emit_far_band) reaches tens of thousands of units sideways
 * from its own span, so a height taken from that span is meaningless once the
 * band has swept out over a different stretch of track. See the ceiling note on
 * TD5_TG_FAR_REACH for what the local version looked like in frame.
 *
 * Computed lazily rather than at generate time because the node list is still
 * being appended to (branch corridors) after the elevation pass; by the time the
 * first band is emitted the list is final. tg_apply_elevation invalidates it, so
 * a second generate in the same process cannot inherit the first one's floor. */
static double s_track_min_y;
static int    s_track_min_valid;

static double tg_track_min_y(const TG_NodeList *nl)
{
    int i;

    if (s_track_min_valid) return s_track_min_y;
    s_track_min_y = (nl->count > 0) ? nl->v[0].y : 0.0;
    for (i = 1; i < nl->count; i++)
        if (nl->v[i].y < s_track_min_y) s_track_min_y = nl->v[i].y;
    s_track_min_valid = 1;
    return s_track_min_y;
}

/* Two summed sines, a raised-cosine hump over each deliberate bridge run, then
 * a global rescale so no span exceeds MAX_GRADE.
 * Mirrors apply_road_elevation() in td5_trackgen.py, plus the bridge humps. */
static void tg_apply_elevation(const TD5_TrackGenSpec *spec, TG_NodeList *nl)
{
    const double max_grade = spec->max_grade_x1000 / 1000.0;
    double amp = (double)spec->elevation_amplitude;
    double ph1, ph2, worst = 0.0;
    int waves, i;

    /* Every y on the track is about to change (or, on the early-out below, has
     * just been built fresh) -- either way the cached global floor is stale. */
    s_track_min_valid = 0;

    if (amp <= 0.0 || nl->count < 3) return;

    waves = tg_range(2, 6);
    ph1 = tg_frand() * 2.0 * TD5_TG_PI;
    ph2 = tg_frand() * 2.0 * TD5_TG_PI;

    for (i = 0; i < nl->count; i++) {
        double f = (double)i / (double)(nl->count - 1);
        nl->v[i].y = amp * (0.6 * sin(2.0 * TD5_TG_PI * waves * f + ph1)
                          + 0.4 * sin(4.0 * TD5_TG_PI * waves * f + ph2));
    }

    /* Deck per deliberate bridge run: flatten the run to its own chord, then
     * add a raised-cosine hump on top.
     *
     * BOTH halves are needed, and the first one is not cosmetic. Hump slope
     * SUPERPOSES on the sine profile's slope. Adding a 0.113-grade hump to a
     * profile already near the 0.120 cap pushed the worst span to 0.169, and
     * tg_apply_elevation's rescale is GLOBAL -- it then multiplied the whole
     * track by 0.708, so switching bridges on silently flattened every hill on
     * the map by 29%. Measured, not hypothetical.
     *
     * Flattening the run to its chord removes the base profile's local slope
     * from the grade budget (and is what a deck looks like anyway -- a bridge
     * spans terrain, it does not undulate over it). The remaining budget is
     * then handed to the hump, whose height is clamped to fit. So no bridge can
     * ever trigger the global rescale, and terrain outside the runs is
     * untouched. */
    {
        int runs = 0, clamped = 0, r0;
        double lowest = TD5_TG_BRIDGE_HEIGHT;
        for (r0 = 0; r0 < nl->count; r0 += TD5_TG_BRIDGE_RUN) {
            int s0 = r0, s1 = r0 + TD5_TG_BRIDGE_RUN - 1, k;
            double yb0, yb1, chord, allowed, hrun;

            if (s1 > nl->count - 1) s1 = nl->count - 1;
            if (s1 <= s0) continue;
            /* One hash per run, so the crown is a fair representative. */
            if (!tg_span_in_bridge_run(s0 + TD5_TG_BRIDGE_RUN / 2)) continue;

            yb0 = nl->v[s0].y;
            yb1 = nl->v[s1].y;
            chord = (yb1 - yb0) / (double)(s1 - s0);
            for (k = s0; k <= s1; k++) {
                double u = (double)(k - s0) / (double)(s1 - s0);
                nl->v[k].y = yb0 + (yb1 - yb0) * u;
            }

            /* Peak slope of a raised cosine of height H over RUN spans is
             * H*PI/RUN, so invert that against the leftover budget. */
            if (max_grade <= 0.0) {
                hrun = TD5_TG_BRIDGE_HEIGHT;      /* no cap configured */
            } else {
                allowed = max_grade * (double)spec->span_length - fabs(chord);
                if (allowed < 0.0) allowed = 0.0;
                hrun = allowed * (double)TD5_TG_BRIDGE_RUN / TD5_TG_PI;
                if (hrun > TD5_TG_BRIDGE_HEIGHT) hrun = TD5_TG_BRIDGE_HEIGHT;
            }

            for (k = s0; k <= s1; k++) {
                double t = ((double)(k - s0) + 0.5) / (double)TD5_TG_BRIDGE_RUN;
                nl->v[k].y += hrun * 0.5 * (1.0 - cos(2.0 * TD5_TG_PI * t));
            }
            runs++;
            if (hrun < TD5_TG_BRIDGE_HEIGHT - 1.0) clamped++;
            if (hrun < lowest) lowest = hrun;
        }
        if (runs)
            TD5_LOG_I(LOG_TAG, "trackgen: %d deliberate bridge run(s) of %d "
                      "spans, crown +%.0f (%d grade-clamped, lowest +%.0f)",
                      runs, TD5_TG_BRIDGE_RUN, TD5_TG_BRIDGE_HEIGHT,
                      clamped, lowest);
    }

    /* Anchor the profile to y=0 at the start line. The grid spawn places cars
     * at y~0 rather than sampling the road, so a track whose span 0 sits at
     * (say) -2040 drops every car into a ~2000-unit free-fall at the green
     * light -- observed as "WHEELS: ----" and the racers never touching down. */
    {
        double y0 = nl->v[0].y;
        for (i = 0; i < nl->count; i++) nl->v[i].y -= y0;
    }

    for (i = 1; i < nl->count; i++) {
        double dy = nl->v[i].y - nl->v[i - 1].y;
        double g  = fabs(dy) / (double)spec->span_length;
        if (g > worst) worst = g;
    }
    if (max_grade > 0.0 && worst > max_grade) {
        double k = max_grade / worst;
        for (i = 0; i < nl->count; i++) nl->v[i].y *= k;
        TD5_LOG_I(LOG_TAG, "trackgen: elevation rescaled by %.3f (grade %.3f -> %.3f)",
                  k, worst, max_grade);
    }
}

/* ------------------------------------------------------ byte emitters ----- */
typedef struct {
    unsigned char *b;
    size_t         len;
    size_t         cap;
    int            oom;
} TG_Buf;

static void tg_buf_free(TG_Buf *buf)
{
    free(buf->b);
    buf->b = NULL;
    buf->len = buf->cap = 0;
}

static int tg_buf_need(TG_Buf *buf, size_t extra)
{
    if (buf->oom) return 0;
    if (buf->len + extra <= buf->cap) return 1;
    {
        size_t cap = buf->cap ? buf->cap : 4096;
        unsigned char *nb;
        while (cap < buf->len + extra) cap *= 2;
        nb = (unsigned char *)realloc(buf->b, cap);
        if (!nb) { buf->oom = 1; return 0; }
        buf->b = nb;
        buf->cap = cap;
    }
    return 1;
}

static void tg_put_u8(TG_Buf *buf, unsigned int v)
{
    if (!tg_buf_need(buf, 1)) return;
    buf->b[buf->len++] = (unsigned char)(v & 0xFF);
}

static void tg_put_u16(TG_Buf *buf, unsigned int v)
{
    if (!tg_buf_need(buf, 2)) return;
    buf->b[buf->len++] = (unsigned char)(v & 0xFF);
    buf->b[buf->len++] = (unsigned char)((v >> 8) & 0xFF);
}

static void tg_put_u32(TG_Buf *buf, unsigned int v)
{
    if (!tg_buf_need(buf, 4)) return;
    buf->b[buf->len++] = (unsigned char)(v & 0xFF);
    buf->b[buf->len++] = (unsigned char)((v >> 8) & 0xFF);
    buf->b[buf->len++] = (unsigned char)((v >> 16) & 0xFF);
    buf->b[buf->len++] = (unsigned char)((v >> 24) & 0xFF);
}

static void tg_put_i32(TG_Buf *buf, int v)
{
    tg_put_u32(buf, (unsigned int)v);
}

static void tg_put_zeros(TG_Buf *buf, size_t n)
{
    if (!tg_buf_need(buf, n)) return;
    memset(buf->b + buf->len, 0, n);
    buf->len += n;
}

/* Copy a file verbatim. Used for the sky: the renderer wants a 256x256 RGBA
 * FORWSKY.png in the level directory, and reusing a shipped panorama is both
 * guaranteed-valid and better looking than anything this generator could
 * synthesise. Forward-only tracks need no BACKSKY -- the loader falls back to
 * FORWSKY when it is absent (td5_game.c:4644-4648). */
static int tg_copy_file(const char *src, const char *dst)
{
    FILE *fi, *fo;
    char buf[16384];
    size_t n;
    int ok = 1;

    fi = fopen(src, "rb");
    if (!fi) return 0;
    fo = fopen(dst, "wb");
    if (!fo) { fclose(fi); return 0; }
    while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) {
        if (fwrite(buf, 1, n, fo) != n) { ok = 0; break; }
    }
    fclose(fi);
    fclose(fo);
    return ok;
}

/* Install a sky by borrowing one from a shipped level, chosen from the seed so
 * different generated tracks get different skies. Candidates are probed in
 * order because not every level ships one. Non-fatal: without it the race just
 * renders against the flat clear colour, which is what happened before. */
static void tg_install_sky(const char *dir, unsigned int seed)
{
    static const int k_sky_levels[] = { 1, 2, 3, 5, 8, 13, 21, 29 };
    const int n = (int)(sizeof(k_sky_levels) / sizeof(k_sky_levels[0]));
    char src[256], dst[320];
    int i;

    for (i = 0; i < n; i++) {
        int lvl = k_sky_levels[(seed / 7u + (unsigned)i) % (unsigned)n];
        snprintf(src, sizeof(src),
                 "re/assets/levels/level%03d/FORWSKY.png", lvl);
        snprintf(dst, sizeof(dst), "%s/FORWSKY.png", dir);
        if (tg_copy_file(src, dst)) {
            TD5_LOG_I(LOG_TAG, "trackgen: sky from level%03d -> %s", lvl, dst);
            return;
        }
    }
    TD5_LOG_W(LOG_TAG, "trackgen: no shipped FORWSKY.png found to borrow; "
              "race will render against the flat clear colour");
}

static int tg_write_file(const char *dir, const char *name,
                         const unsigned char *data, size_t len)
{
    char path[320];
    FILE *f;
    size_t wrote;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) {
        TD5_LOG_E(LOG_TAG, "trackgen: cannot open %s for writing", path);
        return 0;
    }
    wrote = fwrite(data, 1, len, f);
    fclose(f);
    if (wrote != len) {
        TD5_LOG_E(LOG_TAG, "trackgen: short write on %s (%zu/%zu)",
                  path, wrote, len);
        return 0;
    }
    TD5_LOG_I(LOG_TAG, "trackgen: wrote %s (%zu bytes)", path, len);
    return 1;
}

/* Round-to-nearest for signed doubles (lrint is not uniformly available). */
static int tg_round(double v)
{
    return (int)(v >= 0.0 ? (v + 0.5) : (v - 0.5));
}

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

static int tg_branches_enabled(void)
{
    /* Default ON (2026-08-26); set TD5RE_AUTOTRACK_BRANCHES=0 to disable. */
    return td5_env_flag_on("TD5RE_AUTOTRACK_BRANCHES");
}

/* True if span si lies in ANY fork's cleared region (approach through rejoin):
 * the corridor bows into the side<0 lateral, so verge scenery there would sit in
 * the branch. Used by the facade/prop/water suppression. */
static int tg_span_in_fork_clear(int si)
{
    int i;
    for (i = 0; i < s_fork_count; i++)
        if (si >= s_forks[i].F - TD5_TG_BRANCH_WIDEN - 2 &&
            si <= s_forks[i].R + 2)
            return 1;
    return 0;
}

/* [R6 item 10] Largest per-span heading change (radians) over a fork's span
 * range, read from the already-built centreline tangents. A fork narrows the
 * main road to its LEFT half and shifts it a quarter-road toward the inside of
 * the bend (TD5_TG_MAIN_SHIFT) while the branch bows a full road-width the
 * other way; on a tight/acute curve the shifted carriageway folds over its own
 * inner edge, which lifts a car driving it -- the user's "span 570 collision is
 * LIFTING the car" (fork 2 on seed 99991 lands on an acute S-bend) and the
 * paired request to "avoid this kind of curve". The placement loop slides a
 * fork forward until this stays under TD5_TG_FORK_MAX_TURN, i.e. onto a
 * sweeping stretch where the shift cannot fold. Covers the widened approach
 * (F-WIDEN) through the rejoin span (R). */
static double tg_fork_region_max_curve(const TG_NodeList *nl, int F, int L, int ring)
{
    /* Scan exactly the span range the walk-time clamp (tg_span_in_fork_run)
     * gentles, so the logged number reflects what was actually straightened --
     * a span one past either end is ordinary single road (no fork geometry) and
     * its curvature is not a fold risk. */
    int lo = F - TD5_TG_BRANCH_WIDEN;
    int hi = F + 1 + L;
    double worst = 0.0;
    int i;
    if (lo < 1) lo = 1;
    if (hi > ring - 1) hi = ring - 1;
    for (i = lo; i <= hi && i + 1 < nl->count; i++) {
        double d = nl->v[i].tx * nl->v[i + 1].tx + nl->v[i].tz * nl->v[i + 1].tz;
        double a;
        if (d > 1.0) d = 1.0; else if (d < -1.0) d = -1.0;
        a = acos(d);
        if (a > worst) worst = a;
    }
    return worst;
}

/* Append one vertex row of (lanes+1) points for node n, relative to (ox,oy,oz),
 * laterally shifted by `shift` (world units, +ve = left of travel) and using
 * `width`. Returns the row's first vertex index. */
static int tg_append_row(TG_Buf *verts, int *vtx_count, const TG_Node *n,
                         int lanes, double width, double shift,
                         int ox, int oy, int oz)
{
    const double lx = n->tz, lz = -n->tx;
    const int first = *vtx_count;
    int j;
    for (j = 0; j <= lanes; j++) {
        double t = shift + (width * 0.5) - (width * (double)j / (double)lanes);
        int dx = tg_round(n->x + lx * t) - ox;
        int dy = tg_round(n->y) - oy;
        int dz = tg_round(n->z + lz * t) - oz;
        tg_put_u16(verts, (unsigned)(dx & 0xFFFF));
        tg_put_u16(verts, (unsigned)(dy & 0xFFFF));
        tg_put_u16(verts, (unsigned)(dz & 0xFFFF));
    }
    *vtx_count += lanes + 1;
    return first;
}

/* Overwrite fields of an already-emitted span record in place. */
static void tg_patch_span(TG_Buf *spans, int si, int type, int lanes,
                          int lvi, int rvi, int link_next, int link_prev,
                          int ox, int oy, int oz)
{
    unsigned char *r = spans->b + (size_t)si * 24;
    r[0] = (unsigned char)type;
    r[3] = (unsigned char)((TD5_TG_HEIGHT_NIBBLE << 4) | (lanes & 0x0F));
    r[4] = (unsigned char)(lvi & 0xFF);       r[5] = (unsigned char)(lvi >> 8);
    r[6] = (unsigned char)(rvi & 0xFF);       r[7] = (unsigned char)(rvi >> 8);
    r[8] = (unsigned char)(link_next & 0xFF); r[9] = (unsigned char)((link_next >> 8) & 0xFF);
    r[10]= (unsigned char)(link_prev & 0xFF); r[11]= (unsigned char)((link_prev >> 8) & 0xFF);
    {   /* origin: these spans own their rows, so they own their origin too */
        int i, v[3];
        v[0] = ox; v[1] = oy; v[2] = oz;
        for (i = 0; i < 3; i++) {
            unsigned int u = (unsigned int)v[i];
            r[12 + i*4 + 0] = (unsigned char)(u & 0xFF);
            r[12 + i*4 + 1] = (unsigned char)((u >> 8) & 0xFF);
            r[12 + i*4 + 2] = (unsigned char)((u >> 16) & 0xFF);
            r[12 + i*4 + 3] = (unsigned char)((u >> 24) & 0xFF);
        }
    }
}

/* Append a whole span record (used for the pad span and the corridor).
 *
 * `attr` is the surface byte, passed in rather than hardcoded so an appended
 * corridor span carries the SAME surface class as the main-ring span it runs
 * beside (biome grip included) -- the branch is the same road, not a different
 * one.
 *
 * MASK 0, deliberately. The old body wrote `1 | (1 << (lanes - 1))`, i.e. "the
 * two outer lanes use the ALTERNATE surface", copied from shipped level001. On
 * a HALF carriageway br_lanes is 2, so bit0 | bit1 = 0x03 marks BOTH lanes --
 * the whole branch became the 0x10 alternate class, and
 * td5_track_surface_is_slow returns 1 for anything with 0x10 set. That is the
 * reported "some branches spawn with slow lane attribute, like driving on a
 * sidewalk": every lane of the corridor was the slow class while textured like
 * tarmac. tg_emit_span_range was already fixed to write 0 for the main ring
 * (see the note there); this is the same fix for the appended spans, which were
 * missed because they go through a different writer. */
static void tg_append_span(TG_Buf *spans, int type, int attr, int lanes,
                           int lvi, int rvi, int link_next, int link_prev,
                           int ox, int oy, int oz)
{
    tg_put_u8 (spans, (unsigned)type);
    tg_put_u8 (spans, (unsigned)attr);
    tg_put_u8 (spans, 0);
    tg_put_u8 (spans, (unsigned)((TD5_TG_HEIGHT_NIBBLE << 4) | (lanes & 0x0F)));
    tg_put_u16(spans, (unsigned)lvi);
    tg_put_u16(spans, (unsigned)rvi);
    tg_put_u16(spans, (unsigned)(link_next & 0xFFFF));
    tg_put_u16(spans, (unsigned)(link_prev & 0xFFFF));
    tg_put_i32(spans, ox);
    tg_put_i32(spans, oy);
    tg_put_i32(spans, oz);
}

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

static int tg_branch_min_len(void)
{
    return td5_env_int("TD5RE_AUTOTRACK_BRANCH_MINLEN",
                       TD5_TG_BRANCH_MIN_LEN, 8, 400);
}

/* [R6 item 10] Is span si inside a fork's span range (widened approach through
 * rejoin)? Stateless, derived only from si and the deterministic fork placement
 * constants -- the SAME positions the placement loop in tg_emit_strip commits to
 * (pos = grid+120, then R+150; L = max(k_len, min_len)). Used by the centreline
 * walk (via the forward declaration up top) to gentle the curvature there,
 * BEFORE s_forks exists. If the placement loop later drops the last fork (ring-
 * fit / lane check), the extra straightened span range simply carries no fork --
 * gentle road, no harm. Mirrors the bridge-run gate exactly. */
static int tg_span_in_fork_run(int si)
{
    static const int k_lens[3] = { 8, 40, 120 };
    int min_len, pos, i;
    if (!tg_branches_enabled()) return 0;
    min_len = tg_branch_min_len();
    pos = TD5_TG_GRID_SPAN + 120;
    for (i = 0; i < 3; i++) {
        int L = k_lens[i] < min_len ? min_len : k_lens[i];
        int F = pos;
        int R = F + 1 + L;
        /* +/-2 spans of margin past the widened approach and the rejoin so the
         * road eases INTO and OUT of the fork gently rather than meeting a sharp
         * bend right where the carriageways start to split / merge. */
        if (si >= F - TD5_TG_BRANCH_WIDEN - 2 && si <= R + 2) return 1;
        pos = R + 150;
    }
    return 0;
}

/* Lateral centre of the MAIN (left) half carriageway -- constant. */
#define TD5_TG_MAIN_SHIFT(w)   ((w) * 0.25)

/* Bow amplitude (x width) usable over a corridor of `len` spans without the
 * branch centre moving sideways faster than TD5_TG_BRANCH_RATE * span_length
 * per span. d/dk of the half-sine peaks at amp*width*PI/len, so
 * amp <= RATE*span_len*len / (width*PI). Never above TD5_TG_BRANCH_BOW: a long
 * fork should look like the authored one, not sail off across the map. */
static double tg_branch_bow(int len, double width)
{
    double amp;
    if (len <= 0 || width < 1.0) return 0.0;
    amp = TD5_TG_BRANCH_RATE * (double)TD5_TG_SPAN_LENGTH * (double)len
        / (width * TD5_TG_PI);
    return (amp > TD5_TG_BRANCH_BOW) ? TD5_TG_BRANCH_BOW : amp;
}

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

static double tg_fork_sep_for(int fork_index)
{
    static const double k_sep[] = { 0.20, 0.60, 1.00, 0.30 };
    const int n = (int)(sizeof(k_sep) / sizeof(k_sep[0]));
    double s;
    /* Default ON: the user asked for various separation sizes. =0 pins the old
     * uniform widest (sep 1.0) for an A/B against the pre-item-10 behaviour. */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_BRANCH_VARY_SEP")) return 1.0;
    if (fork_index < 0) fork_index = 0;
    s = k_sep[(unsigned)fork_index % (unsigned)n];
    if (s < TD5_TG_BRANCH_SEP_MIN) s = TD5_TG_BRANCH_SEP_MIN;
    return s;
}

/* True where fork `fork_index` is tight enough to read as a divided avenue
 * (item 9: "a distinction of different dividers ... if it is a branch
 * representing an avenue"). Above the threshold the carriageways diverge into
 * two separate roads and a central island would just float in open ground. */
static int tg_fork_is_avenue(int fork_index)
{
    return tg_fork_sep_for(fork_index) <= TD5_TG_AVENUE_SEP_MAX;
}

/* Lateral centre of the BRANCH (right) half carriageway at corridor step k with
 * separation scale `sep`: the right-half centre (-width/4) plus an outward bow
 * that is 0 at both ends (so it lines back up with the road halves at the fork
 * and the rejoin) and peaks in the middle, scaled by `sep`.
 *
 * SEMANTIC CHANGE 2026-08-26: the bow amplitude is length-derived
 * (tg_branch_bow) rather than the flat TD5_TG_BRANCH_BOW. 2026-08-28 (item 10):
 * split out the sep-scaled form; tg_branch_shift is now the sep=1.0 (widest)
 * case, which is what the tunnel-bore enclosure wants (it must cover the WIDEST
 * a corridor could be). Sign convention is unchanged (negative = right of
 * travel), so every reader keeps working. */
static double tg_branch_shift_s(int k, int len, double width, double sep)
{
    double f   = (len > 0) ? (double)k / (double)len : 0.0;   /* 0 .. 1 */
    double bow = sin(f * TD5_TG_PI);                          /* 0 -> 1 -> 0 */
    return -width * 0.25 - width * tg_branch_bow(len, width) * sep * bow;
}

static double tg_branch_shift(int k, int len, double width)
{
    return tg_branch_shift_s(k, len, width, 1.0);
}

/* ---- branch lane GAIN (feedback: "branches should be able to spawn and
 * widen their lanes") ----
 * The old corridor was a fixed half carriageway for its whole length: br_lanes
 * lanes at wscale 0.5, start to finish. A real alternate route opens out once
 * it is clear of the split.
 *
 * So the corridor now RAMPS: it must be exactly the half carriageway at k=0
 * and at k=len-1 (those two spans are where it lines up with the fork span and
 * the type-11 rejoin, both of which are full width and unchanged), and in
 * between it gains up to TD5RE_AUTOTRACK_BRANCH_GAIN extra lanes on a half sine
 * -- same shape as the bow, so the widening and the divergence peak together.
 *
 * Per-span lane counts are legal on these spans because appended corridor spans
 * own their vertex rows (see the BRANCHES design note); no shared row changes
 * point count. Default ON, TD5RE_AUTOTRACK_BRANCH_WIDEN=0 to pin the old fixed
 * half width. */
static int tg_branch_widen_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_BRANCH_WIDEN");
}

/* Extra lanes at corridor step k as a CONTINUOUS quantity, 0 at both ends.
 *
 * Split out from tg_branch_lane_gain (2026-08-27) because the two consumers
 * want different things and conflating them is what produced the reported
 * "sudden changes of lane widths on right track branches". The strip row needs
 * an INTEGER point count -- a row has a whole number of points -- but the road
 * WIDTH has no such constraint, and taking the width from the rounded lane
 * count quantised it to 0.5 / 0.75 / 1.0 of the road: three discrete widths
 * with a hard step at whichever span the rounding tipped over. */
static double tg_branch_gain_f(int k, int len, int base_lanes)
{
    int gain_max;
    double f;
    if (!tg_branch_widen_enabled() || len < 4) return 0.0;
    if (k <= 0 || k >= len - 1) return 0.0;
    gain_max = td5_env_int("TD5RE_AUTOTRACK_BRANCH_GAIN", 2, 0, 4);
    /* Keep the widened corridor inside the lane range the rail LUTs, edge masks
     * and suspension paths are exercised in (see td5_trackgen_apply_config's
     * 2..4 clamp): never take the branch past 4 lanes. */
    if (base_lanes + gain_max > 4) gain_max = 4 - base_lanes;
    if (gain_max <= 0) return 0.0;
    /* Half sine over the INTERIOR of the corridor, so the widening grows in and
     * out over several spans instead of stepping at k=1. */
    f = (double)(k - 1) / (double)(len - 3 > 0 ? len - 3 : 1);
    return sin(f * TD5_TG_PI) * (double)gain_max;
}

/* Extra lanes at corridor step k (0 at both ends), rounded to a whole lane.
 * SUBDIVISION ONLY: this is the row's point count, not its width. */
static int tg_branch_lane_gain(int k, int len, int base_lanes)
{
    const double gf = tg_branch_gain_f(k, len, base_lanes);
    int g = (int)(gf + 0.5);
    if (g < 0) g = 0;
    return g;
}

/* Width scale of the branch carriageway at step k: 0.5 (the half road) plus a
 * quarter road per gained lane -- CONTINUOUS, so the widening is a taper the
 * eye reads as a smooth opening-out rather than a step at a span boundary.
 *
 * SEMANTIC CHANGE 2026-08-27: this used to quantise through tg_branch_lane_gain
 * and return one of {0.50, 0.75, 1.00}; it now returns any value in [0.5, 1.0].
 * Signature and meaning ("multiply the road's full width by this to get the
 * branch carriageway's width at step k") are unchanged, and the new value is
 * never larger than the old one by more than half a lane, so a reader using it
 * for CLEARANCE is still conservative. A reader that assumed the three discrete
 * values would now disagree. */
static double tg_branch_wscale(int k, int len, int base_lanes)
{
    return 0.5 + 0.25 * tg_branch_gain_f(k, len, base_lanes);
}

/* Separation-aware forms (item 10). The branch row is centred on tg_branch_shift
 * and extends +/- half its width, so WIDENING grows the carriageway INWARD as
 * well as outward. On a WIDE split there is room -- the bow has carried the
 * branch far enough right that its inner edge still clears the main road. On a
 * tight AVENUE there is not: gaining a lane would push the inner edge back
 * across the slim median into the oncoming main carriageway. So an avenue keeps
 * the FIXED half carriageway (a constant 2+2 divided road, which is what an
 * avenue actually is); only diverging forks open out. Both the width and the
 * point count are gated the SAME way, so the strip rows and the road mesh stay
 * point-for-point identical however the fork is classified. */
static double tg_branch_wscale_s(int k, int len, int base_lanes, double sep)
{
    if (sep <= TD5_TG_AVENUE_SEP_MAX) return 0.5;
    return tg_branch_wscale(k, len, base_lanes);
}

static int tg_branch_lane_gain_s(int k, int len, int base_lanes, double sep)
{
    if (sep <= TD5_TG_AVENUE_SEP_MAX) return 0;
    return tg_branch_lane_gain(k, len, base_lanes);
}

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

/* Half the MAIN road's width at span si, taking the wider of the span's two
 * ends so a width ramp never reports less road than the span actually has. */
static double tg_road_half_width(const TG_NodeList *nl, int si)
{
    double w;
    if (!nl || si < 0 || si >= nl->count) return 0.0;
    w = nl->v[si].width;
    if (si + 1 < nl->count && nl->v[si + 1].width > w) w = nl->v[si + 1].width;
    return w * 0.5;
}

/* Outermost drivable lateral at span si on `side`, as a POSITIVE distance from
 * the main centerline. Never less than the main road's own half width. */
static double tg_carriageway_reach(const TG_NodeList *nl, int si, double side)
{
    double reach = tg_road_half_width(nl, si);
    int i;

    if (side >= 0.0) return reach;            /* no corridor bows LEFT */
    if (!tg_branches_enabled()) return reach;
    if (!nl || si < 0 || si + 1 >= nl->count) return reach;

    for (i = 0; i < s_fork_count; i++) {
        const int F = s_forks[i].F, L = s_forks[i].len;
        const int lanes = nl->v[si].lanes;
        const int br    = lanes - lanes / 2;   /* branch half, as tg_emit_strip */
        int k, e;
        /* One span of slack past each mouth: at the fork and the rejoin the
         * corridor is still lined up with the road, but a caller asking about
         * the mouth span itself must not see a narrower answer than its
         * neighbour or scenery pops in for one span. */
        if (si < F - 1 || si > F + L + 1) continue;
        k = si - F - 1;
        if (k < 0) k = 0;
        if (k > L) k = L;
        /* BOTH ends of the span: the bow and the widening grow across it, so
         * the near end alone under-reports. */
        for (e = 0; e <= 1; e++) {
            const int    kk = (k + e > L) ? L : k + e;
            const int    ni = (si + e < nl->count) ? si + e : si;
            const double w  = nl->v[ni].width;
            /* Branch centre is NEGATIVE (right of travel); its outer edge is a
             * further half carriageway-width right of that. Uses the fork's OWN
             * separation (item 10) so a tight avenue reports a nearer reach than
             * a wide split and scenery clears the branch at its ACTUAL width. */
            const double out = -tg_branch_shift_s(kk, L, w, s_forks[i].sep)
                             + w * tg_branch_wscale_s(kk, L, br, s_forks[i].sep) * 0.5;
            if (out > reach) reach = out;
        }
    }
    return reach;
}

/* Is `lateral` on (or within `margin` of) any carriageway at span si? */
static int tg_on_carriageway(const TG_NodeList *nl, int si, double lateral,
                             double margin)
{
    const double side = (lateral >= 0.0) ? 1.0 : -1.0;
    const double lat  = (lateral >= 0.0) ? lateral : -lateral;
    return lat <= tg_carriageway_reach(nl, si, side) + margin;
}

/* Push a setback measured from the MAIN ROAD EDGE out far enough that nothing
 * standing on it overlaps a carriageway. Returns `gap` unchanged where the road
 * edge is already the outermost tarmac (which is every span on the +ve lateral,
 * and every span off a fork). */
static double tg_carriageway_clear_gap(const TG_NodeList *nl, int si,
                                       double side, double gap, double margin)
{
    const double need = tg_carriageway_reach(nl, si, side)
                      - tg_road_half_width(nl, si) + margin;
    return (gap < need) ? need : gap;
}

/* [R3 item 17] Geometry-safety validation pass. "Add additional checks to avoid
 * geometry of floor and walls getting in the way of the road."
 *
 * This is a read-only diagnostic run once the strip is built. It cannot MOVE
 * geometry (the meshes are already authored to clear the carriageway through
 * tg_carriageway_clear_gap / tg_side_blocked), so its job is to make the two
 * failure modes that DO put a floor on the road VISIBLE as a number in race.log
 * instead of a silent hole you can only find by driving into it:
 *
 *   1. ROAD SELF-OVERLAP. The centreline is an OPEN wandering walk
 *      (tg_build_centerline); when the section placer runs out of retries
 *      escaping a cul-de-sac it forces a straight and can leave the road passing
 *      back over an EARLIER stretch -- two road floors sharing the same ground.
 *      Flagged when two NON-neighbour spans' road surfaces actually overlap
 *      (centre distance < 0.8*(half_i+half_j)); a clean hairpin keeps its two
 *      sides a full road-width apart (the curvature-safety floor guarantees it)
 *      so it does NOT trip this.
 *   2. SUSPECT FORK REACH. The shared carriageway authority
 *      (tg_carriageway_reach) must never report a reach narrower than the plain
 *      road nor an implausibly wide one; either means a fork put drivable
 *      surface where scenery (or another carriageway) is placed. Uses the
 *      authority read-only -- no fork arithmetic is re-derived here.
 *
 * Scoped to the MAIN RING (s_ring_len): appended branch corridors legitimately
 * run beside the main road and would false-positive the overlap test. */
static void tg_validate_geometry_safety(const TG_NodeList *nl, int nspans)
{
    int i, j, ring, overlaps = 0, bad_reach = 0, checked = 0;
    double span_len;

    if (!nl || nl->count < 3 || nspans < 3) return;
    ring = (s_ring_len > 0 && s_ring_len < nspans) ? s_ring_len : nspans;
    if (ring + 1 > nl->count) ring = nl->count - 1;
    if (ring < 3) return;

    /* Approx span spacing from the first step, to size the "legitimately near"
     * exclusion window: spans within a few of each other are ALWAYS close and
     * are not an overlap. */
    {
        const double dx = nl->v[1].x - nl->v[0].x;
        const double dz = nl->v[1].z - nl->v[0].z;
        span_len = sqrt(dx * dx + dz * dz);
        if (span_len < 1.0) span_len = 1.0;
    }

    for (i = 0; i < ring; i++) {
        const double hi = tg_road_half_width(nl, i);
        const double rr = tg_carriageway_reach(nl, i, -1.0);
        if (rr < hi - 1.0 || rr > hi * 6.0 + 1.0) {
            if (bad_reach < 8)
                TD5_LOG_W(LOG_TAG, "geometry-safety: span %d carriageway reach "
                          "%.0f vs road half %.0f (suspect fork geometry)",
                          i, rr, hi);
            bad_reach++;
        }
    }

    for (i = 0; i < ring; i++) {
        const double xi = nl->v[i].x, zi = nl->v[i].z;
        const double hi = tg_road_half_width(nl, i);
        /* Exclusion window: pairs this close along the path are neighbours and
         * are supposed to be near each other. Sized off the wider of the two
         * roads so a wide road's neighbours are never mistaken for an overlap. */
        for (j = i + 1; j < ring; j++) {
            const double hj = tg_road_half_width(nl, j);
            const int window = (int)((hi + hj) / span_len) + 6;
            double dx, dz, d2, lim;
            if (j - i <= window) continue;
            dx = nl->v[j].x - xi; dz = nl->v[j].z - zi;
            d2 = dx * dx + dz * dz;
            lim = (hi + hj) * 0.8;
            checked++;
            if (d2 < lim * lim) {
                if (overlaps < 8)
                    TD5_LOG_W(LOG_TAG, "geometry-safety: road self-overlap spans "
                              "%d<->%d (dist %.0f < %.0f)", i, j, sqrt(d2), lim);
                overlaps++;
            }
        }
    }

    if (overlaps || bad_reach)
        TD5_LOG_W(LOG_TAG, "geometry-safety: %d road-overlap pair(s), %d suspect "
                  "fork span(s) over %d main-ring spans (%d pairs tested)",
                  overlaps, bad_reach, ring, checked);
    else
        TD5_LOG_I(LOG_TAG, "geometry-safety: clean -- 0 road overlaps, 0 suspect "
                  "fork spans over %d main-ring spans (%d pairs tested)",
                  ring, checked);
}

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
static const char *const k_guard_kind_name[TG_GK_COUNT] = {
    "other", "skirt", "road", "branch-road", "tunnel", "gantry", "end-wall",
    "deck", "water", "coast", "decal", "city", "block", "cross", "flora",
    "park-tree", "terrain", "building", "prop", "rail", "branch-side"
};
enum { TG_GKC_SCENERY = 0, TG_GKC_EXEMPT = 1, TG_GKC_DECAL = 2,
       TG_GKC_UNDER = 3 };
/* [R8] Is this exempt kind's licence SPAN-LOCAL? A bridge deck, a bore, a
 * gantry, a road quad and an end wall are all authored for ONE span and reach
 * barely past it, so their licence is scoped to a span run and cannot cover a
 * mesh that lands somewhere else.
 *
 * MEASURED EXCEPTION -- the ground skirt, the sea plane and the coastline are
 * legitimately TENS OF THOUSANDS of units wide. On seed 99991 the skirt emitted
 * for span 627 reaches the carriageway at span 647 where the road doubles back
 * on itself; that is the ground doing its job, not a stray. Scoping those would
 * have made the guard reject the ground under the road -- caught by the DIAG
 * dump before it shipped, and the reason this is a per-kind property rather
 * than one global rule. */
static const unsigned char k_guard_kind_local[TG_GK_COUNT] = {
    0,  /* other       */
    0,  /* skirt       -- wide by design, see above */
    1,  /* road        */
    1,  /* branch-road */
    1,  /* tunnel      */
    1,  /* gantry      */
    1,  /* end-wall    */
    1,  /* deck        */
    0,  /* water       -- a sea plane spans the whole seaward side */
    0,  /* coast       */
    1,  /* decal       */
    0, 0, 0, 0, 0,                 /* city block cross flora park-tree */
    0,                             /* terrain -- wide by design, see above */
    0, 0, 0, 0                     /* building prop rail branch-side */
};
static const unsigned char k_guard_kind_class[TG_GK_COUNT] = {
    TG_GKC_SCENERY,  /* other       */
    TG_GKC_UNDER,    /* skirt       */
    TG_GKC_EXEMPT,   /* road        */
    TG_GKC_EXEMPT,   /* branch-road */
    TG_GKC_EXEMPT,   /* tunnel      */
    TG_GKC_EXEMPT,   /* gantry      */
    TG_GKC_EXEMPT,   /* end-wall    */
    TG_GKC_EXEMPT,   /* deck        */
    TG_GKC_UNDER,    /* water       */
    TG_GKC_UNDER,    /* coast       */
    TG_GKC_DECAL,    /* decal       */
    TG_GKC_SCENERY,  /* city        */
    TG_GKC_SCENERY,  /* block       */
    TG_GKC_SCENERY,  /* cross       */
    TG_GKC_SCENERY,  /* flora       */
    TG_GKC_SCENERY,  /* park-tree   */
    /* [R8] Ground, like the skirt: legal under the road, never on top of it.
     * Rejecting a terrain apron that sits BELOW an elevated road (seed 777
     * spans 860..904, dy -9900..-500) would punch a hole under the track; the
     * same emitter putting grass ON the road is R7 item 19 and must still go. */
    TG_GKC_UNDER,    /* terrain     */
    TG_GKC_SCENERY,  /* building    */
    TG_GKC_SCENERY,  /* prop        */
    TG_GKC_SCENERY,  /* rail        */
    TG_GKC_SCENERY   /* branch-side */
};

static size_t s_guard_ex_lo[TD5_TG_GUARD_EX_MAX];
static size_t s_guard_ex_hi[TD5_TG_GUARD_EX_MAX];
static unsigned char s_guard_ex_kind[TD5_TG_GUARD_EX_MAX];
static int    s_guard_ex_si[TD5_TG_GUARD_EX_MAX];
static int    s_guard_ex_n;
static long   s_guard_rejects;          /* running total across the whole build */
static long   s_guard_residual;         /* on-road meshes STILL present post-drop */
static long   s_guard_rej_kind[TG_GK_COUNT];   /* [R8] breakdown by kind */
static long   s_guard_ex_scope_hits;    /* [R8] exempt marks that fell out of scope */

/* Default ON -- this is the fix, not an opt-in. TD5RE_R7_GUARD=0 pins the old
 * unguarded output for an A/B, and TD5RE_R7_GUARD_REPORT=1 makes it report-only
 * (log but keep the bytes) for diagnosing what it would drop. */
static int tg_guard_enabled(void) { return td5_env_flag_on("TD5RE_R7_GUARD"); }
/* Report-only is an opt-in (default OFF): log what WOULD be dropped but keep the
 * bytes, for diagnosing the guard without changing the geometry. */
static int tg_guard_report_only(void)
{
    return td5_env_flag_off("TD5RE_R7_GUARD_REPORT");
}
/* [R8] The precision pass (area coverage + kind classes + span-scoped
 * exemptions). Default ON; TD5RE_R8_GUARD=0 pins the R7 vertex-only test for a
 * single-variable A/B. */
static int tg_guard_r8(void) { return td5_env_flag_on("TD5RE_R8_GUARD"); }
/* [R8] TD5RE_R8_GUARD_DIAG=<span>: dump every mesh of the entry containing that
 * span with its kind, coverage, height band and verdict. 0 = off. */
static int tg_guard_diag_span(void)
{
    return td5_env_int("TD5RE_R8_GUARD_DIAG", 0, 0, 100000);
}

static void tg_guard_ex_reset(void) { s_guard_ex_n = 0; }

/* Record [lo, hi) of the current entry's mesh buffer as geometry of `kind`
 * emitted for span `si`. Called at the emit sites in tg_emit_models; ranges the
 * emitters never mark fall through as TG_GK_OTHER, which is SCENERY -- the
 * fail-safe direction (validated, not licensed). */
static void tg_guard_mark(size_t lo, size_t hi, int kind, int si)
{
    if (hi <= lo) return;
    if (s_guard_ex_n < TD5_TG_GUARD_EX_MAX) {
        s_guard_ex_lo[s_guard_ex_n] = lo;
        s_guard_ex_hi[s_guard_ex_n] = hi;
        s_guard_ex_kind[s_guard_ex_n] = (unsigned char)kind;
        s_guard_ex_si[s_guard_ex_n] = si;
        s_guard_ex_n++;
    }
}

/* Kind of the mesh at byte offset `off`, plus the span it was marked for.
 * The NARROWEST matching mark wins. Marks NEST -- an emitter group's range in
 * tg_emit_models encloses the specific ranges its own emitters marked inside it
 * (the decal, for one) -- and the specific one is the answer, whichever order
 * the two were recorded in. */
static int tg_guard_kind_of(size_t off, int *pmark_si)
{
    int i, kind = TG_GK_OTHER;
    size_t best = (size_t)-1;
    if (pmark_si) *pmark_si = -1;
    for (i = 0; i < s_guard_ex_n; i++)
        if (off >= s_guard_ex_lo[i] && off < s_guard_ex_hi[i]) {
            const size_t w = s_guard_ex_hi[i] - s_guard_ex_lo[i];
            if (w <= best) {
                best = w;
                kind = (int)s_guard_ex_kind[i];
                if (pmark_si) *pmark_si = s_guard_ex_si[i];
            }
        }
    return kind;
}

/* Little-endian scalar reads over the assembled mesh bytes. */
static float tg_rd_f32(const unsigned char *p)
{
    unsigned int u = (unsigned int)p[0] | ((unsigned int)p[1] << 8)
                   | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}
static unsigned int tg_rd_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static unsigned int tg_rd_u16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* Byte length of the mesh whose 0x38 header starts at b[off]. The header stores
 * the vertex-block offset (0x30) and the de-indexed vertex count (0x08), and the
 * vertices are the last thing in the record, so end = vtx_off + count*44. Returns
 * 0 if the header does not look like one of ours (fail safe: caller keeps it). */
static size_t tg_guard_mesh_len(const unsigned char *b, size_t off, size_t cap)
{
    unsigned int vtxoff, vtxcnt;
    if (off + TD5_TG_MESH_DISK_SIZE > cap) return 0;
    if (tg_rd_u16(b + off) != 259) return 0;
    vtxcnt = tg_rd_u32(b + off + 0x08);
    vtxoff = tg_rd_u32(b + off + 0x30);
    if (vtxoff < TD5_TG_MESH_DISK_SIZE) return 0;
    return (size_t)vtxoff + (size_t)vtxcnt * TD5_TG_VTX_SIZE;
}

/* Nearest main-ring node to world (wx,wz), searched in a window around the
 * entry's spans so a far-band vertex reaching tens of thousands of units OUT
 * still resolves to the span it stands beside, not a curve several spans away. */
static int tg_guard_nearest_node(const TG_NodeList *nl, int lo, int hi,
                                 double wx, double wz)
{
    int i, best = lo;
    double bd = 1e300;
    for (i = lo; i <= hi; i++) {
        double dx = wx - nl->v[i].x, dz = wz - nl->v[i].z;
        double d2 = dx * dx + dz * dz;
        if (d2 < bd) { bd = d2; best = i; }
    }
    return best;
}

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

/* Is a quad with this intrusion, height band and policy class illegal? */
static int tg_guard_quad_bad(int cls, double intr, double dy_lo, double dy_hi)
{
    if (intr <= TD5_TG_GUARD_PEN)        return 0;   /* edge-hugging: legal */
    if (dy_hi < -TD5_TG_GUARD_UNDER)     return 0;   /* buried under the road */
    if (cls == TG_GKC_EXEMPT)            return 0;   /* authored across it */
    if (cls == TG_GKC_DECAL)                         /* legal only flush */
        return !(dy_lo >= -TD5_TG_GUARD_FLUSH && dy_hi <= TD5_TG_GUARD_FLUSH);
    /* [R8 item 7] UNDER: the ground skirt, the sea plane and the coastline are
     * ALLOWED to reach across the carriageway -- they are the surface it is laid
     * on -- but only from BELOW. This is the discrimination R8 item 7 turned out
     * to need. MEASURED: seed 99991's skirt for spans 643..647 lies over the
     * carriageway at spans 626..629 at dy +250..+450 (the road doubles back on
     * itself at a different height), which is the user's "tiles spilling over
     * the road around 627" exactly. A blanket skirt exemption licenses that; a
     * span-scoped one would instead have rejected the perfectly good ground the
     * same emitter lays UNDER the road. Height is the axis that separates them. */
    if (cls == TG_GKC_UNDER)
        return dy_hi > TD5_TG_GUARD_FLUSH;
    if (dy_lo > TD5_TG_GUARD_OVERHEAD)   return 0;   /* clears it overhead */
    return 1;
}

/* Accumulate one candidate group's verdict into the running worst. `bad` quads
 * outrank clean ones outright, so the reported hit always explains the verdict. */
static void tg_guard_hit_keep(TG_GuardHit *h, int *have_bad, int bad,
                              double intr, double dy_lo, double dy_hi, int si)
{
    if (*have_bad && !bad) return;
    if (bad && !*have_bad) { *have_bad = 1; h->intr = -1e300; }
    if (intr > h->intr) {
        h->intr = intr; h->dy_lo = dy_lo; h->dy_hi = dy_hi; h->si = si;
    }
}

/* Scan the mesh at `off` against the carriageway under policy class `cls`.
 * Returns 1 when some part of it is illegal, filling *hit with the worst
 * offending group. When nothing is illegal *hit still carries the worst
 * intrusion seen, which is what the DIAG dump prints. */
static int tg_guard_mesh_scan(const TG_NodeList *nl, int ring,
                              const unsigned char *b, size_t off, size_t mlen,
                              int win_lo, int win_hi, int cls, TG_GuardHit *hit)
{
    const int    tag    = (int)tg_rd_u16(b + off + 0x02);
    const unsigned int vtxcnt = tg_rd_u32(b + off + 0x08);
    const unsigned int vtxoff = tg_rd_u32(b + off + 0x30);
    const int r8 = tg_guard_r8();
    TG_GuardHit worst;
    unsigned int vi;
    int bad = 0, have_bad = 0;

    worst.intr = -1e300; worst.dy_lo = 0.0; worst.dy_hi = 0.0; worst.si = -1;
    /* Two running maxima, because the widest-covering quad of a mesh is often
     * NOT the illegal one (a terrain apron's deepest quad can be buried under
     * the road while a different quad of the same mesh stands on it). Reporting
     * the first would have made the DIAG dump lie about why a mesh was dropped,
     * which is the kind of half-truth that produced the diagnosis this round had
     * to re-measure. Once anything is illegal, only illegal quads compete. */

    if (tag) {
        /* Billboard. The vertices are LOCAL offsets about a 24.8 world origin
         * and the quad is turned at the camera every frame, so its footprint is
         * not a quad at all -- it is a DISC of radius max|local x| about the
         * origin. Testing it as a disc is both cheaper and orientation-correct;
         * the R7 code added local x to world X, which only held where the road
         * happened to run along Z. */
        const double ox = (double)tg_rd_f32(b + off + 0x1C) / 256.0;
        const double oy = (double)tg_rd_f32(b + off + 0x20) / 256.0;
        const double oz = (double)tg_rd_f32(b + off + 0x24) / 256.0;
        double half = 0.0, top = 0.0, bot = 0.0;
        double lateral, rp, rn, cover, pen, roady;
        int si;
        for (vi = 0; vi < vtxcnt; vi++) {
            const unsigned char *vp = b + off + vtxoff + (size_t)vi * TD5_TG_VTX_SIZE;
            double lx, ly;
            if ((size_t)(vp + 12 - b) > off + mlen) break;
            lx = (double)tg_rd_f32(vp);
            ly = (double)tg_rd_f32(vp + 4);
            if (lx < 0.0) lx = -lx;
            if (lx > half) half = lx;
            if (ly > top)  top = ly;
            if (ly < bot)  bot = ly;
        }
        if (!r8) {
            /* R7 fallback path, kept BYTE-FAITHFUL: R7 added the billboard's
             * local x straight onto world X and tested each resulting point.
             * That is only correct where the road runs along Z, but the A/B
             * knob is worthless unless it reproduces what R7 actually did. */
            for (vi = 0; vi < vtxcnt; vi++) {
                const unsigned char *vp = b + off + vtxoff
                                        + (size_t)vi * TD5_TG_VTX_SIZE;
                double vx, vy, vz, lat, r, d;
                int s2;
                if ((size_t)(vp + 12 - b) > off + mlen) break;
                vx = ox + (double)tg_rd_f32(vp);
                vy = oy + (double)tg_rd_f32(vp + 4);
                vz = oz + (double)tg_rd_f32(vp + 8);
                s2 = tg_guard_nearest_node(nl, win_lo, win_hi, vx, vz);
                if (s2 < 0 || s2 >= ring) continue;
                if (vy > nl->v[s2].y + TD5_TG_GUARD_OVERHEAD) continue;
                if (vy < nl->v[s2].y - TD5_TG_GUARD_UNDER)    continue;
                lat = (vx - nl->v[s2].x) * nl->v[s2].tz
                    - (vz - nl->v[s2].z) * nl->v[s2].tx;
                r = tg_carriageway_reach(nl, s2, (lat >= 0.0) ? 1.0 : -1.0);
                d = r - (lat >= 0.0 ? lat : -lat);
                if (d > worst.intr) {
                    worst.intr = d; worst.si = s2;
                    worst.dy_lo = worst.dy_hi = vy - nl->v[s2].y;
                }
            }
            bad = (worst.intr > TD5_TG_GUARD_PEN && cls != TG_GKC_EXEMPT);
            if (hit) *hit = worst;
            return bad;
        }
        si = tg_guard_nearest_node(nl, win_lo, win_hi, ox, oz);
        if (si < 0 || si >= ring) { if (hit) *hit = worst; return 0; }
        roady = nl->v[si].y;
        lateral = (ox - nl->v[si].x) * nl->v[si].tz
                - (oz - nl->v[si].z) * nl->v[si].tx;
        rp = tg_carriageway_reach(nl, si, 1.0);
        rn = tg_carriageway_reach(nl, si, -1.0);
        cover = (lateral + half < rp ? lateral + half : rp)
              - (lateral - half > -rn ? lateral - half : -rn);
        pen = (lateral >= 0.0 ? rp : rn) - (lateral >= 0.0 ? lateral : -lateral);
        if (!r8 || cover < pen) cover = pen;
        bad = tg_guard_quad_bad(cls, cover, oy + bot - roady, oy + top - roady);
        tg_guard_hit_keep(&worst, &have_bad, bad, cover,
                          oy + bot - roady, oy + top - roady, si);
        if (hit) *hit = worst;
        return bad;
    }

    /* Opaque geometry: de-indexed QUADS, which is what every writer in this file
     * emits (road strip, box faces, facade cells, decals). Walking them four at
     * a time is what makes the lateral INTERVAL meaningful -- a per-mesh bounding
     * box would fuse a building's near and far walls into one slab and a whole
     * tunnel entry would read as covering the road. */
    for (vi = 0; vi < vtxcnt; vi += 4) {
        /* EACH VERTEX IS MEASURED IN ITS OWN SPAN'S FRAME, and its lateral is
         * NORMALISED by that span's own reach (t = +-1 is the road edge). A
         * quad spans a whole road segment, so its far corner belongs to the
         * next node; projecting all four into one frame put a lateral error of
         * (span length x curvature) on the near corner, and on a bend that read
         * as ~750 units of false coverage. It rejected 34 perfectly good verge
         * bands on seed 777 -- every one of them at si = mark_si+1, which is
         * what gave the artefact away. Normalising per vertex removes the frame
         * error AND handles a mesh whose corners resolve to different spans,
         * which is exactly the doubling-back case this guard has to judge. */
        double t[4], dy[4];
        double t_lo, t_hi, dy_lo, dy_hi, r_rep = 0.0, t_best = 1e300;
        double cover, pen;
        int k, n = 0, si0 = -1;

        for (k = 0; k < 4 && vi + (unsigned)k < vtxcnt; k++) {
            const unsigned char *vp = b + off + vtxoff
                                    + (size_t)(vi + (unsigned)k) * TD5_TG_VTX_SIZE;
            double vx, vy, vz, lateral, r;
            int si;
            if ((size_t)(vp + 12 - b) > off + mlen) break;
            vx = (double)tg_rd_f32(vp);
            vy = (double)tg_rd_f32(vp + 4);
            vz = (double)tg_rd_f32(vp + 8);
            si = tg_guard_nearest_node(nl, win_lo, win_hi, vx, vz);
            if (si < 0 || si >= ring) continue;
            lateral = (vx - nl->v[si].x) * nl->v[si].tz
                    - (vz - nl->v[si].z) * nl->v[si].tx;
            r = tg_carriageway_reach(nl, si, (lateral >= 0.0) ? 1.0 : -1.0);
            if (!(r > 0.0)) continue;
            t[n] = lateral / r;
            dy[n] = vy - nl->v[si].y;
            r_rep += r;
            /* Report the span the mesh reaches DEEPEST into, not an arbitrary
             * corner's: that is the span the user is standing on. */
            if (si0 < 0 || (lateral >= 0.0 ? t[n] : -t[n]) < t_best) {
                t_best = (lateral >= 0.0 ? t[n] : -t[n]);
                si0 = si;
            }
            n++;
        }
        if (n <= 0) continue;
        r_rep /= (double)n;
        t_lo = t_hi = t[0];
        dy_lo = dy_hi = dy[0];
        pen = 0.0;
        for (k = 0; k < n; k++) {
            double d;
            if (t[k] < t_lo) t_lo = t[k];
            if (t[k] > t_hi) t_hi = t[k];
            if (dy[k] < dy_lo) dy_lo = dy[k];
            if (dy[k] > dy_hi) dy_hi = dy[k];
            /* R7's per-vertex penetration, height-gated per vertex as it was. */
            if (dy[k] > TD5_TG_GUARD_OVERHEAD || dy[k] < -TD5_TG_GUARD_UNDER)
                continue;
            d = (1.0 - (t[k] >= 0.0 ? t[k] : -t[k])) * r_rep;
            if (d > pen) pen = d;
        }
        /* Road half-widths cancel: the overlap is computed in t and scaled back
         * by the representative reach, so it reads in world units as before. */
        cover = ((t_hi < 1.0 ? t_hi : 1.0) - (t_lo > -1.0 ? t_lo : -1.0)) * r_rep;
        if (!r8 || cover < pen) cover = pen;
        {   /* keep scanning: the DIAG dump wants the WORST offending quad,
             * not the first one found. */
            const int qbad = tg_guard_quad_bad(cls, cover, dy_lo, dy_hi);
            tg_guard_hit_keep(&worst, &have_bad, qbad, cover, dy_lo, dy_hi, si0);
            if (qbad) bad = 1;
        }
    }
    if (hit) *hit = worst;
    return bad;
}

/* R7-shaped wrapper kept for the residual self-check and the exempt-scope test:
 * "is this mesh illegal under class `cls`", reporting depth and span. */
static int tg_guard_mesh_on_road_cls(const TG_NodeList *nl, int ring,
                                     const unsigned char *b, size_t off,
                                     size_t mlen, int win_lo, int win_hi,
                                     int cls, double *pdepth, int *psi)
{
    TG_GuardHit hit;
    int bad = tg_guard_mesh_scan(nl, ring, b, off, mlen, win_lo, win_hi,
                                 cls, &hit);
    if (bad) {
        if (pdepth) *pdepth = hit.intr;
        if (psi)    *psi = hit.si;
    }
    return bad;
}


/* Validate one entry's assembled meshes against the carriageway and drop the
 * ones standing in the road. Rewrites `meshes` and `moff`/`*pnmesh` in place
 * (compacting left, which is always safe: we only remove). Returns the number
 * rejected. Corridor-only entries (past the ring) carry only exempt branch road
 * and are skipped. */
/* >= TG_MAX_MESHES_PER_ENTRY (4 spans * 96 = 384); a literal so it does not
 * depend on TD5_TG_SPANS_PER_ENTRY, which is defined further down. */
#define TD5_TG_GUARD_KEPT_MAX 512
static int tg_guard_validate_entry(const TG_NodeList *nl, int ring, int s0,
                                   int ns, TG_Buf *meshes, size_t *moff,
                                   int *pnmesh)
{
    const int nmesh = *pnmesh;
    unsigned char *b = meshes->b;
    /* Per KEPT mesh, was it exempt (authored on/over the road)? Needed for the
     * residual self-check, which must count only NON-exempt on-road meshes --
     * exempt ranges are original byte offsets and stop mapping once compaction
     * moves the bytes. */
    static unsigned char kept_exempt[TD5_TG_GUARD_KEPT_MAX];  /* policy class */
    int win_lo, win_hi, i, nn = 0, rejected = 0;
    size_t w = 0;

    if (!tg_guard_enabled() || nmesh <= 0 || ring < 3) return 0;
    if (s0 >= ring) return 0;

    win_lo = s0 - TD5_TG_GUARD_WINDOW; if (win_lo < 0) win_lo = 0;
    win_hi = s0 + ns - 1 + TD5_TG_GUARD_WINDOW;
    if (win_hi > ring - 1) win_hi = ring - 1;
    if (win_hi < win_lo) return 0;

    for (i = 0; i < nmesh; i++) {
        const size_t off = moff[i];
        size_t mlen = (off <= meshes->len) ? tg_guard_mesh_len(b, off, meshes->len) : 0;
        int keep = 1, exempt, kind = TG_GK_OTHER, cls = TG_GKC_SCENERY;
        int mark_si = -1;
        double depth = 0.0;
        int si = -1;

        if (mlen == 0 || off + mlen > meshes->len) {
            /* Unparseable / spans to end: keep verbatim, do not risk corruption. */
            mlen = (i + 1 < nmesh ? moff[i + 1] : meshes->len) - off;
            exempt = 1;
        } else {
            kind = tg_guard_kind_of(off, &mark_si);
            cls  = (int)k_guard_kind_class[kind];
            /* R7 fallback (TD5RE_R8_GUARD=0): the two classes R8 introduced
             * collapse back to what R7 did -- a decal was ordinary scenery that
             * simply never tripped the vertex test, and the skirt/water/coast
             * were unconditionally exempt. */
            if (!tg_guard_r8() && cls == TG_GKC_DECAL) cls = TG_GKC_SCENERY;
            if (!tg_guard_r8() && cls == TG_GKC_UNDER) cls = TG_GKC_EXEMPT;
            exempt = (cls == TG_GKC_EXEMPT);
            /* [R8] SPAN-SCOPED EXEMPTION. An exempt mark licenses geometry that
             * crosses the carriageway AT THE SPAN IT WAS EMITTED FOR. Test the
             * mesh as plain SCENERY first: if it does intrude, the licence only
             * stands when the intrusion is near the marking span. Anything
             * further is an unrelated mesh that happened to land inside an
             * exempt byte range, and it is validated normally. */
            if (exempt && tg_guard_r8() && mark_si >= 0 &&
                k_guard_kind_local[kind] &&
                tg_guard_mesh_on_road_cls(nl, ring, b, off, mlen, win_lo, win_hi,
                                          TG_GKC_SCENERY, &depth, &si)) {
                int d = si - mark_si; if (d < 0) d = -d;
                if (d > TD5_TG_GUARD_EX_SPANS) {
                    cls = TG_GKC_SCENERY;
                    exempt = 0;
                    s_guard_ex_scope_hits++;
                }
            }
            depth = 0.0; si = -1;
            if (!exempt &&
                tg_guard_mesh_on_road_cls(nl, ring, b, off, mlen, win_lo, win_hi,
                                          cls, &depth, &si)) {
                keep = 0;
                rejected++;
                s_guard_rejects++;
                s_guard_rej_kind[kind]++;
                tg_acct(TG_ACCT_GUARD_REJECT, si);
                tg_acct(TG_ACCT_R8_GUARD, si);
                if (rejected <= 8)
                    TD5_LOG_W(LOG_TAG, "on-road guard: rejected %s mesh at span "
                              "%d (coverage %.0f, %u verts)",
                              k_guard_kind_name[kind], si, depth,
                              tg_rd_u32(b + off + 0x08));
            }
        }

        /* [R8] DIAG: dump every mesh of one entry with its measured footprint.
         * This is the instrument the R8 diagnosis was made with -- it is what
         * showed that the surviving objects at spans 187/627/686 were NOT
         * height-gated and NOT exempt, but wide quads whose corners all sat
         * outside the road. Kept so the next round re-measures instead of
         * inheriting this conclusion in turn. */
        {
            const int dsp = tg_guard_diag_span();
            /* Report by WHERE THE MESH LANDS, not by which entry emitted it.
             * Scenery for span 184 routinely reaches span 187, so dumping only
             * the entry that contains the complaint span would miss the very
             * mesh the user is looking at. TD5RE_R8_GUARD_AUDIT=1 instead dumps
             * EVERY mesh on the whole strip that covers the carriageway at all,
             * whatever its class -- the sweep the acceptance bar asks for, and
             * the only way to see what the exemptions are currently licensing. */
            const int audit = td5_env_flag_off("TD5RE_R8_GUARD_AUDIT");
            if ((audit || (dsp > 0 && dsp >= win_lo && dsp <= win_hi))
                && mlen > 0 && off + mlen <= meshes->len) {
                TG_GuardHit h;
                int badv = tg_guard_mesh_scan(nl, ring, b, off, mlen, win_lo,
                                              win_hi, cls, &h);
                int d = (dsp > 0) ? h.si - dsp : 0;
                if (d < 0) d = -d;
                if (audit ? (h.intr > TD5_TG_GUARD_PEN
                             && tg_guard_quad_bad(TG_GKC_SCENERY, h.intr,
                                                  h.dy_lo, h.dy_hi))
                          : (d <= 2 && h.intr > -1e299))
                    TD5_LOG_I(LOG_TAG, "guard-diag: entry@%d mesh %d kind=%s "
                              "cls=%d mark_si=%d verts=%u intr=%.0f si=%d "
                              "dy=[%.0f,%.0f] %s", s0, i,
                              k_guard_kind_name[kind], cls, mark_si,
                              tg_rd_u32(b + off + 0x08), h.intr, h.si, h.dy_lo,
                              h.dy_hi, badv ? "REJECT" : "keep");
            }
        }

        /* Drop the bytes only on a real rejection AND when NOT in report-only
         * mode; otherwise (accepted, exempt, unparseable, or report-only) keep
         * the mesh, compacting it left over any gap left by earlier drops. */
        if (keep || tg_guard_report_only()) {
            if (w != off) memmove(b + w, b + off, mlen);
            /* Record the POLICY CLASS, not a bare exempt bit: the residual
             * self-check has to judge each kept mesh by the same rule the drop
             * pass did, and a decal legally covers the road. */
            if (nn < TD5_TG_GUARD_KEPT_MAX)
                kept_exempt[nn] = (unsigned char)(exempt ? TG_GKC_EXEMPT : cls);
            moff[nn++] = w;
            w += mlen;
        }
    }

    meshes->len = w;
    *pnmesh = nn;

    /* Residual self-check: re-validate the kept NON-exempt meshes. This is the
     * class-level cleanliness proof -- after the drop, zero non-exempt geometry
     * may stand on the road. A non-zero residual is a parse/compaction bug (not
     * a policy call) and is surfaced in the build summary. In report-only mode
     * the rejected meshes were kept, so a residual there equals the reject count
     * and is expected. */
    if (!tg_guard_report_only()) {
        for (i = 0; i < nn; i++) {
            const size_t off = moff[i];
            size_t mlen = tg_guard_mesh_len(b, off, meshes->len);
            double depth = 0.0;
            int si = -1;
            int cls = (i < TD5_TG_GUARD_KEPT_MAX) ? (int)kept_exempt[i]
                                                  : TG_GKC_SCENERY;
            if (cls == TG_GKC_EXEMPT) continue;
            if (mlen == 0 || off + mlen > meshes->len) continue;
            if (tg_guard_mesh_on_road_cls(nl, ring, b, off, mlen, win_lo, win_hi,
                                          cls, &depth, &si))
                s_guard_residual++;
        }
    }

    return rejected;
}

/* ===================== [S1] RANGE EMITTER =====================
 * Emit spans [first_span, first_span+span_count) and their vertex rows into
 * CALLER-OWNED buffers, with vertex indices continuing from *vtx_count.
 *
 * This exists for Phase 2 streaming (docs/plans/AUTOTRACK_STREAMING.md): a
 * rolling ring buffer has to rewrite a REGION of the track, not rebuild the
 * whole thing, so the emitter has to be addressable by range.
 *
 * CONTRACT: first_span MUST be block-aligned (first_span % block == 0), and
 * span_count SHOULD be a multiple of block except for the final partial one.
 * Origin blocks share an origin and share rows across the block, so a range
 * starting mid-block would produce a different origin for those spans and
 * different vertex indices -- silently different geometry, not an error. The
 * streaming design already advances the write cursor in block units for this
 * reason. Asserted below rather than assumed.
 */
static int tg_emit_span_range(const TG_NodeList *nl, int first_span,
                              int span_count, int lanes, int block,
                              TG_Buf *spans, TG_Buf *verts, int *vtx_count)
{
    const int row_pts = lanes + 1;
    const int range_end = first_span + span_count;
    int s0, ok = 1;

    if (block <= 0 || span_count <= 0) return 0;
    if (first_span % block != 0) {
        TD5_LOG_E(LOG_TAG, "trackgen: emit range first_span=%d is not aligned "
                  "to the %d-span origin block; refusing (would silently "
                  "change geometry)", first_span, block);
        return 0;
    }
    if (range_end > nl->count - 1) {
        TD5_LOG_E(LOG_TAG, "trackgen: emit range [%d,%d) exceeds %d spans",
                  first_span, range_end, nl->count - 1);
        return 0;
    }

    for (s0 = first_span; s0 < range_end; s0 += block) {
        const int ns  = (s0 + block <= range_end) ? block : (range_end - s0);
        const int ox  = tg_round(nl->v[s0].x);
        const int oy  = tg_round(nl->v[s0].y);
        const int oz  = tg_round(nl->v[s0].z);
        const int base = *vtx_count;
        int k;

        if (*vtx_count + (ns + 1) * row_pts > TD5_TG_MAX_VERTICES) {
            TD5_LOG_W(LOG_TAG, "trackgen: vertex ceiling hit at span %d "
                      "(%d verts); truncating", s0, *vtx_count);
            break;
        }

        /* ns+1 rows, one per node s0..s0+ns inclusive, all relative to this
         * block's origin. The shared row at a block seam is re-emitted under
         * the new origin, exactly as shipped tracks do. */
        for (k = 0; k <= ns; k++) {
            const TG_Node *n = &nl->v[s0 + k];
            const double lx = n->tz, lz = -n->tx;
            int j;
            for (j = 0; j < row_pts; j++) {
                double t  = (n->width * 0.5)
                          - (n->width * (double)j / (double)lanes);
                int dx = tg_round(n->x + lx * t) - ox;
                int dy = tg_round(n->y) - oy;
                int dz = tg_round(n->z + lz * t) - oz;
                if (dx < -32768 || dx > 32767 || dy < -32768 || dy > 32767 ||
                    dz < -32768 || dz > 32767) {
                    TD5_LOG_E(LOG_TAG, "trackgen: vertex offset out of int16 "
                              "range in block at span %d (%d,%d,%d) -- reduce "
                              "the origin block size", s0, dx, dy, dz);
                    ok = 0;
                }
                tg_put_u16(verts, (unsigned)(dx & 0xFFFF));
                tg_put_u16(verts, (unsigned)(dy & 0xFFFF));
                tg_put_u16(verts, (unsigned)(dz & 0xFFFF));
            }
            *vtx_count += row_pts;
        }

        for (k = 0; k < ns; k++) {
            tg_put_u8 (spans, 1);                        /* span_type QUAD_A */
            tg_put_u8 (spans, (unsigned)tg_surface_attr(s0 + k));
            /* Lane bitmask 0 = every lane is the low-nibble surface (dry asphalt,
         * full grip). The old `1 | (1<<(lanes-1))` marked the OUTER lanes, which
         * surface_type_for_span_lane turns into the 0x10 "alternate/off-road"
         * surface -- td5_track_surface_is_slow returns 1 for anything with 0x10
         * set, so those lanes silently SLOWED the car while textured identically
         * to the fast lanes (reported as "lanes that make the car slower look the
         * same as the road"). A generated arcade road is uniformly drivable. */
        tg_put_u8 (spans, 0);
            tg_put_u8 (spans, (unsigned)((TD5_TG_HEIGHT_NIBBLE << 4)
                                         | (lanes & 0x0F)));
            tg_put_u16(spans, (unsigned)(base + k * row_pts));
            tg_put_u16(spans, (unsigned)(base + (k + 1) * row_pts));
            tg_put_u16(spans, 0xFFFF);                   /* link_next = -1 */
            tg_put_u16(spans, 0xFFFF);                   /* link_prev = -1 */
            tg_put_i32(spans, ox);
            tg_put_i32(spans, oy);
            tg_put_i32(spans, oz);
        }
    }
    return ok && !spans->oom && !verts->oom;
}

/* [S1 GATE] Prove the range emitter is composable: emitting the track in
 * block-aligned CHUNKS must be byte-identical to emitting it in one call. If
 * that does not hold, streaming would silently produce different geometry from
 * the same seed. Runs only when TD5RE_AUTOTRACK_SELFCHECK=1. */
static void tg_selfcheck_ranges(const TG_NodeList *nl, int lanes, int block)
{
    const int nspans = nl->count - 1;
    TG_Buf one_s, one_v, many_s, many_v;
    int vc_one = 0, vc_many = 0, s0, ok = 1;
    int chunk = block * 5;      /* several blocks per chunk, still aligned */

    memset(&one_s, 0, sizeof(one_s));   memset(&one_v, 0, sizeof(one_v));
    memset(&many_s, 0, sizeof(many_s)); memset(&many_v, 0, sizeof(many_v));

    if (!tg_emit_span_range(nl, 0, nspans, lanes, block, &one_s, &one_v,
                            &vc_one))
        ok = 0;

    for (s0 = 0; ok && s0 < nspans; s0 += chunk) {
        int n = (s0 + chunk <= nspans) ? chunk : (nspans - s0);
        if (!tg_emit_span_range(nl, s0, n, lanes, block, &many_s, &many_v,
                                &vc_many))
            ok = 0;
    }

    if (!ok) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: range emit FAILED");
    } else if (one_s.len != many_s.len || one_v.len != many_v.len ||
               vc_one != vc_many) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: FAIL size (spans %zu vs %zu, "
                  "verts %zu vs %zu, vtx %d vs %d)", one_s.len, many_s.len,
                  one_v.len, many_v.len, vc_one, vc_many);
    } else if (memcmp(one_s.b, many_s.b, one_s.len) != 0) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: FAIL span bytes differ");
    } else if (memcmp(one_v.b, many_v.b, one_v.len) != 0) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: FAIL vertex bytes differ");
    } else {
        TD5_LOG_I(LOG_TAG, "trackgen selfcheck: PASS -- %d spans emitted in "
                  "%d-span chunks is byte-identical to one shot (%zu span B, "
                  "%zu vtx B, %d verts)", nspans, chunk, one_s.len, one_v.len,
                  vc_one);
    }

    tg_buf_free(&one_s);  tg_buf_free(&one_v);
    tg_buf_free(&many_s); tg_buf_free(&many_v);
}

static int tg_emit_strip(const TG_NodeList *nl, TG_Buf *out, int *out_spans)
{
    const int nspans = nl->count - 1;
    TG_Buf spans, verts;
    int vtx_count = 0, ok = 1;

    memset(&spans, 0, sizeof(spans));
    memset(&verts, 0, sizeof(verts));

    /* Lane count is uniform (see tg_build_centerline), so every row has the
     * same point count and consecutive spans can SHARE a row: span i spans
     * rows i and i+1. Shipped tracks do exactly this (level001: span0
     * lvi=0 rvi=5, span1 lvi=5 rvi=10). Emitting a private duplicated row
     * pair per span is geometrically identical but breaks span-to-span
     * adjacency, which resolve_neighbor (td5_track.c:4013) detects by vertex
     * INDEX -- contact then fails at every seam and the car sinks through the
     * road (measured: wheel_mask=0 for 343/385 ticks, even dead flat). */
    const int lanes   = nl->v[0].lanes;
    /* Reset per-call: a failed or branch-less build must not leave stale fork
     * records from a previous generation in the header. */
    s_fork_count = 0;
    s_ring_len = 0;
    /* Spans per shared origin. Tunable so the effect of origin granularity on
     * ground contact is measurable: the ground probe appears to read a span's
     * origin_y (a track starting at y=-2040 probed -522240 = -2040*256), and a
     * shared origin_y makes the collision ground a flat step at the block base
     * while the visible road ramps away from it on hills. */
    const int block = td5_env_int("TD5RE_AUTOTRACK_BLOCK",
                                  TD5_TG_ORIGIN_BLOCK, 1, 20);

    /* ORIGIN BLOCKS. The loader resolves BOTH of a span's vertex rows against
     * THAT span's origin, so two spans can only share a row if they share an
     * origin. Hence origins are per-BLOCK, constant across the block, with the
     * shared row duplicated at block seams -- which is exactly what shipped
     * tracks do (level001 span0 lvi=0 rvi=5, span1 lvi=5 rvi=10).
     *
     * Storing one origin per span while sharing rows displaces every span's FAR
     * edge by one span step, skewing the road surface: contact then flickers
     * between all-four-wheels and none, and the car gets flung down-track
     * (measured: wall_clear median -583186 while driving, though a perfect 2250
     * at rest -- the giveaway that only the far row was wrong).
     *
     * Block length is bounded by the int16 vertex offset: TD5_TG_ORIGIN_BLOCK
     * spans of span_length, plus half the widest road, must stay under 32767. */
    /* [S1] Whole track = one block-aligned range. The emitter below can write
     * any block-aligned range into caller buffers, which is what Phase 2
     * streaming needs to rewrite a region in place. */
    if (!tg_emit_span_range(nl, 0, nspans, lanes, block, &spans, &verts,
                            &vtx_count))
        ok = 0;

    /* ---- BRANCHES (opt-in): multiple forks, each a split-and-rejoin ---- */
    {
        const int ring = (int)(spans.len / 24);   /* main ring span count */
        s_fork_count = 0;

        if (ok && tg_branches_enabled()) {
            const int main_half = lanes / 2;
            const int br_lanes  = lanes - main_half;
            /* Varied corridor lengths give the shipped topologies: a short
             * chicane, a canonical split, a long alternate route. The first
             * entry USED to be 8 spans, which is the "very small branch" that
             * glitched -- lengths are now floored at tg_branch_min_len(). */
            static const int k_lens[] = { 8, 40, 120 };
            const int min_len = tg_branch_min_len();
            int off = ring;                          /* append cursor after ring */
            int pos = TD5_TG_GRID_SPAN + 120;        /* first fork, past the grid */
            unsigned int i;

            for (i = 0; i < sizeof(k_lens) / sizeof(k_lens[0]) &&
                        s_fork_count < TD5_TG_BRANCH_MAX; i++) {
                int L = k_lens[i] < min_len ? min_len : k_lens[i];
                int F = pos;
                int R = F + 1 + L;
                if (main_half < 1 || br_lanes < 1) break;
                if (R + 24 >= ring) break;           /* must fit on the ring */
                /* [R6 item 10] Verify the walk-time straightening (tg_span_in_fork_run
                 * clamp) actually gentled this fork's span range: a fork left on a
                 * sharp bend folds its shifted/bowed carriageways and lifts a car
                 * (the "span 570" report). Should read <= TD5_TG_FORK_MAX_TURN. */
                TD5_LOG_I(LOG_TAG, "trackgen: fork %u F=%d L=%d region maxcurve=%.4f "
                          "(cap %.3f)", i, F, L,
                          tg_fork_region_max_curve(nl, F, L, ring),
                          TD5_TG_FORK_MAX_TURN);
                s_forks[s_fork_count].F = F;
                s_forks[s_fork_count].len = L;
                s_forks[s_fork_count].cbase = off + 1;  /* pad@off, corridor off+1.. */
                s_forks[s_fork_count].R = R;
                /* Separation is keyed to the fork's ORDINAL, not its span, so it
                 * is stable across a regen and varied across the track. */
                s_forks[s_fork_count].sep = tg_fork_sep_for(s_fork_count);
                s_fork_count++;
                off += 1 + L;
                pos = R + 150;                        /* gap before the next fork */
            }

            /* Emit each fork's strip pieces. Corridors are appended in fork
             * order, so their indices match the cbase computed above. */
            for (i = 0; ok && i < (unsigned)s_fork_count; i++) {
                const int F = s_forks[i].F, L = s_forks[i].len;
                const int b0 = s_forks[i].cbase, R = s_forks[i].R;
                const int sentinel_end = b0 + L - 1;
                int k;

                /* 1. FORK span F: full width, type 8, link_next -> corridor. */
                {
                    const TG_Node *a = &nl->v[F], *b = &nl->v[F + 1];
                    int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                    int lvi = tg_append_row(&verts, &vtx_count, a, lanes,
                                            a->width, 0.0, ox, oy, oz);
                    int rvi = tg_append_row(&verts, &vtx_count, b, lanes,
                                            b->width, 0.0, ox, oy, oz);
                    tg_patch_span(&spans, F, 8, lanes, lvi, rvi, b0, -1, ox, oy, oz);
                }
                /* 2. MAIN half carriageway [F+1 .. F+L]. */
                for (k = 1; k <= L; k++) {
                    const int si = F + k;
                    const TG_Node *a = &nl->v[si], *b = &nl->v[si + 1];
                    int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                    int lvi = tg_append_row(&verts, &vtx_count, a, main_half,
                                            a->width * 0.5, TD5_TG_MAIN_SHIFT(a->width),
                                            ox, oy, oz);
                    int rvi = tg_append_row(&verts, &vtx_count, b, main_half,
                                            b->width * 0.5, TD5_TG_MAIN_SHIFT(b->width),
                                            ox, oy, oz);
                    tg_patch_span(&spans, si, 1, main_half, lvi, rvi, -1, -1,
                                  ox, oy, oz);
                }
                /* 3. PAD span at b0-1 (the current append head). */
                {
                    const TG_Node *a = &nl->v[F];
                    int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                    int lvi = tg_append_row(&verts, &vtx_count, a, br_lanes,
                                            a->width * 0.5, 0.0, ox, oy, oz);
                    int rvi = tg_append_row(&verts, &vtx_count, a, br_lanes,
                                            a->width * 0.5, 0.0, ox, oy, oz);
                    tg_append_span(&spans, 1, tg_surface_attr(F), br_lanes,
                                   lvi, rvi, -1, -1, ox, oy, oz);
                }
                /* 4. BRANCH corridor b0..b0+L-1 (right half, bowed, and gaining
                 *    lanes over its interior -- see tg_branch_lane_gain). Each
                 *    row's point count comes from the SAME helper the road mesh
                 *    uses, so strip and mesh cannot drift apart. */
                for (k = 0; k < L; k++) {
                    const TG_Node *a = &nl->v[F + 1 + k], *b = &nl->v[F + 2 + k];
                    const double bsep = s_forks[i].sep;
                    const int ln = br_lanes + tg_branch_lane_gain_s(k, L, br_lanes, bsep);
                    const double wn = tg_branch_wscale_s(k, L, br_lanes, bsep);
                    const int lnf = br_lanes + tg_branch_lane_gain_s(k + 1, L, br_lanes, bsep);
                    const double wf = tg_branch_wscale_s(k + 1, L, br_lanes, bsep);
                    int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                    /* A span's two rows must have the same POINT COUNT (they are
                     * the two edges of one quad grid), so a span where the gain
                     * rounds up uses the finer subdivision for both rows.
                     *
                     * Their WIDTHS are independent, and giving both rows the
                     * wider end's width -- which is what this did until
                     * 2026-08-27 -- is what produced the reported "sudden
                     * changes of lane widths on right track branches": the
                     * corridor became a staircase of constant-width spans that
                     * stepped wherever the rounding tipped over. Each row now
                     * carries its OWN width, so consecutive spans meet at
                     * identical outer points (the collision rails are row points
                     * 0 and lanes_here) and the widening reads as a taper. */
                    const int lanes_here = (lnf > ln) ? lnf : ln;
                    int lvi = tg_append_row(&verts, &vtx_count, a, lanes_here,
                                            a->width * wn,
                                            tg_branch_shift_s(k, L, a->width, bsep),
                                            ox, oy, oz);
                    int rvi = tg_append_row(&verts, &vtx_count, b, lanes_here,
                                            b->width * wf,
                                            tg_branch_shift_s(k + 1, L, b->width, bsep),
                                            ox, oy, oz);
                    int type = (k == 0) ? 9 : ((k == L - 1) ? 10 : 1);
                    int nxt  = (k == L - 1) ? R : -1;
                    int prv  = (k == 0) ? F : -1;
                    tg_append_span(&spans, type, tg_surface_attr(F + 1 + k),
                                   lanes_here, lvi, rvi, nxt, prv, ox, oy, oz);
                }
                /* 5. REJOIN span R: full width, type 11, link_prev -> sentinel. */
                {
                    const TG_Node *a = &nl->v[R], *b = &nl->v[R + 1];
                    int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                    int lvi = tg_append_row(&verts, &vtx_count, a, lanes,
                                            a->width, 0.0, ox, oy, oz);
                    int rvi = tg_append_row(&verts, &vtx_count, b, lanes,
                                            b->width, 0.0, ox, oy, oz);
                    if (R >= 0 && R < ring)
                        tg_patch_span(&spans, R, 11, lanes, lvi, rvi, -1,
                                      sentinel_end, ox, oy, oz);
                }
                TD5_LOG_I(LOG_TAG, "trackgen: fork %u F=%d len=%d corridor=%d..%d "
                          "rejoin=%d sep=%.2f%s (ring=%d)", i, F, L, b0,
                          sentinel_end, R, s_forks[i].sep,
                          tg_fork_is_avenue((int)i) ? " AVENUE" : "", ring);
            }
        }
        s_ring_len = ring;
    }

    if (spans.oom || verts.oom) {
        TD5_LOG_E(LOG_TAG, "trackgen: out of memory building strip");
        ok = 0;
    }

    if (ok) {
        const int emitted = (int)(spans.len / 24);
        const unsigned int vtx_off =
            (unsigned int)(TD5_TG_SPAN_OFFSET + 24 * emitted);

        tg_put_u32(out, TD5_TG_SPAN_OFFSET);
        /* Ring length = the MAIN ROAD span count. With a branch, `emitted`
         * also counts the pad + corridor spans, which must sit OUTSIDE the
         * ring -- that is what makes branch spans normalize through the jump
         * table. Without a branch the two are identical.
         *
         * Shipped level001 writes span_count-1 but it is a CIRCUIT, where the
         * last span closes onto the first. Copying that made the walker wrap
         * backward off span 0 to the ring end (span_raw oscillated 0 <-> 1798
         * every tick, 952 times in one run). */
        tg_put_u32(out, (unsigned int)(s_ring_len > 0 ? s_ring_len : emitted));
        tg_put_u32(out, vtx_off);
        tg_put_u32(out, (unsigned int)vtx_count);
        tg_put_u32(out, (unsigned int)emitted);   /* total spans */
        /* Jump-entry count at 0x14, then N 6-byte records [lo,hi,base] from 0x18
         * (native TD5 offset; the loader iterates all N). The block must stay
         * exactly TD5_TG_PRE_SPAN_BYTES long -- the loader derives the span count
         * from (vtx_off - span_off)/24 -- so pad out the remainder. */
        {
            int j, nrec = s_fork_count;
            if (nrec > (TD5_TG_PRE_SPAN_BYTES - 4) / 6)
                nrec = (TD5_TG_PRE_SPAN_BYTES - 4) / 6;
            tg_put_u32(out, (unsigned int)nrec);
            for (j = 0; j < nrec; j++) {
                tg_put_u16(out, (unsigned)s_forks[j].cbase);
                tg_put_u16(out, (unsigned)(s_forks[j].cbase + s_forks[j].len - 1));
                tg_put_u16(out, (unsigned)(s_forks[j].F + 1));
            }
            tg_put_zeros(out, TD5_TG_PRE_SPAN_BYTES - 4 - nrec * 6);
        }
        if (!tg_buf_need(out, spans.len + verts.len)) {
            ok = 0;
        } else {
            memcpy(out->b + out->len, spans.b, spans.len);
            out->len += spans.len;
            memcpy(out->b + out->len, verts.b, verts.len);
            out->len += verts.len;
        }
        if (out->len != (size_t)vtx_off + (size_t)vtx_count * 6) {
            TD5_LOG_E(LOG_TAG, "trackgen: strip size mismatch (%zu vs %u)",
                      out->len, (unsigned)(vtx_off + vtx_count * 6));
            ok = 0;
        }
        *out_spans = emitted;
        TD5_LOG_I(LOG_TAG, "trackgen: strip = %d spans, %d vertices, %zu bytes",
                  emitted, vtx_count, out->len);
    }

    tg_buf_free(&spans);
    tg_buf_free(&verts);
    return ok && !out->oom;
}

/* ---------------------------------------------------- LEFT/RIGHT.TRK ----- */
/* 3 bytes per span, exactly ring-length rows (short tables are what causes the
 * out-of-bounds read documented at td5_track.c:3503).
 *   byte0 = lateral corridor position, 0 = left rail .. 255 = right rail
 *   byte1 = local heading, 128 = straight ahead
 *   byte2 = authored corner-speed cap, 255 = uncapped (curvature governs)
 */
static int tg_emit_routes(const TG_NodeList *nl, int nspans,
                          int lateral, TG_Buf *out)
{
    int i;
    for (i = 0; i < nspans; i++) {
        /* `nspans` is the TOTAL strip span count, which with branches includes
         * each fork's pad + corridor tail -- but the centerline only has the
         * main-ring nodes, so nl->v[i] for i >= nl->count READ PAST THE ARRAY
         * (uninitialised realloc slack at best, heap overread at worst) and put
         * garbage headings in the table the AI steers by. Map an appended span
         * to the main node its corridor runs beside instead; the pad span, which
         * has no geometry of its own, takes the last real node. */
        int ni = i;
        if (ni > nl->count - 2) {
            int ck = 0, fi = tg_fork_of_corridor(i, &ck);
            ni = (fi >= 0) ? s_forks[fi].F + 1 + ck : nl->count - 2;
            if (ni > nl->count - 2) ni = nl->count - 2;
            if (ni < 0) ni = 0;
        }
        /* byte1 is the ABSOLUTE 12-bit heading, not a deflection: the engine
         * recovers it as heading = (byte * 0x102C) >> 8 (td5_ai.c:1280, and the
         * same formula in ai_route_heading_for_actor at td5_ai.c:305), so invert
         * exactly that. Yaw convention is forward = (sin h, cos h) --
         * td5_physics.c, cited at td5_ai.c:1306 -- hence atan2(tx, tz).
         *
         * Getting this wrong is not subtle: encoding a deflection here (128 =
         * straight) spawned every car pointing the wrong way, and with
         * auto-throttle they drove backwards off the start line into the void. */
        double h = atan2(nl->v[ni].tx, nl->v[ni].tz) * 4096.0 / (2.0 * TD5_TG_PI);
        int h12 = tg_round(h) & 0xFFF;
        int hb  = tg_round((double)h12 * 256.0 / 4140.0);   /* 4140 = 0x102C */

        /* byte < 4 is a junction-zone sentinel, not a heading. The axis offset
         * (TD5_TG_AXIS_HEADING) keeps us clear of it; clamp as a backstop. */
        if (hb < 4)   hb = 4;
        if (hb > 253) hb = 253;

        tg_put_u8(out, (unsigned)lateral);
        tg_put_u8(out, (unsigned)hb);
        tg_put_u8(out, 255);
    }
    return !out->oom;
}

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

/* Finish span for a ring of `ring` main-road spans, or -1 if the ring is too
 * short to hold a grid, a race and a run-off. Pure function of the ring length
 * and the fork table, so the LEVELINF writer and the banner emitter agree
 * without either of them storing it. */
static int tg_finish_span(int ring)
{
    int runoff = td5_env_int("TD5RE_AUTOTRACK_RUNOFF", TD5_TG_RUNOFF_SPANS,
                             0, 600);
    int lo = TD5_TG_GRID_SPAN + 60;      /* shortest race worth having */
    int fs, guard;

    if (ring <= lo) return -1;
    /* A short track cannot afford the full run-off; give it what is left after
     * the minimum race distance rather than refusing to place a finish. */
    if (ring - runoff <= lo) runoff = ring - lo;
    fs = ring - runoff;

    /* Never put the finish line inside a fork's split region (the line would
     * cross two separated half carriageways, so a banner over it would either
     * miss the road or straddle the gore) nor inside a tunnel run (the gantry
     * would stand through the roof). Walk it back to clear ground. */
    for (guard = 0; guard < ring && fs > lo &&
         (tg_span_in_fork_clear(fs) || tg_span_in_tunnel(fs)); guard++)
        fs--;
    if (fs <= lo) return -1;
    return fs;
}

/* ------------------------------------------------------ LEVELINF.DAT ----- */
/* 100 bytes. The level loader reads DWORD[0] (1 = circuit, else
 * point-to-point) and keeps the rest as opaque environment config. */
static int tg_emit_levelinf(const TD5_TrackGenSpec *spec, int nspans,
                            TG_Buf *out)
{
    /* Checkpoints are compared against the NORMALIZED main-ring span, so they
     * are placed on the ring -- not on `nspans`, which also counts the appended
     * corridors. s_ring_len is set by tg_emit_strip, which always runs first. */
    const int ring = (s_ring_len > 0) ? s_ring_len : nspans;
    const int finish = tg_finish_span(ring);
    int cp_count = 4, i;
    int cp_span[7];

    for (i = 0; i < 7; i++) cp_span[i] = 0;
    if (ring < 200) cp_count = 2;
    if (finish > 0) {
        /* Evenly spaced from the grid to the finish, LAST one exactly on the
         * finish span -- that crossing is what ends the race. */
        const int span0 = TD5_TG_GRID_SPAN;
        for (i = 0; i < cp_count; i++) {
            cp_span[i] = span0 + (int)((long)(finish - span0) * (i + 1)
                                       / cp_count);
            tg_acct(TG_ACCT_CHECKPOINT, cp_span[i]);
        }
        TD5_LOG_I(LOG_TAG, "trackgen: finish span %d of ring %d (%d spans of "
                  "run-off past the line), %d checkpoints",
                  finish, ring, ring - finish, cp_count);
    } else {
        /* Ring too short for a grid + race + run-off: fall back to the old
         * proportional placement rather than shipping a track that cannot be
         * finished. */
        for (i = 0; i < cp_count; i++)
            cp_span[i] = (int)((long)ring * (i + 1) / (cp_count + 1));
        TD5_LOG_W(LOG_TAG, "trackgen: ring %d too short to place a finish with "
                  "run-off; using proportional checkpoints", ring);
    }

    tg_put_u32(out, spec->circuit ? 1u : 0u);   /* 0x00 track_type */
    tg_put_u32(out, 1);                         /* 0x04 smoke_enable */
    tg_put_u32(out, (unsigned)cp_count);        /* 0x08 checkpoint_count */
    for (i = 0; i < 7; i++)                     /* 0x0C checkpoint_spans */
        tg_put_u32(out, (unsigned)cp_span[i]);
    tg_put_u32(out, 2);                         /* 0x28 weather_type */
    tg_put_u32(out, 0);                         /* 0x2C density_pair_count */
    tg_put_u32(out, 0);                         /* 0x30 traffic_enable */
    tg_put_zeros(out, 24);                      /* 0x34 density_pairs 12xu16 */
    tg_put_zeros(out, 8);                       /* 0x4C pad */
    /* 0x54 sky_animation_index. Shipped circuits use 36, point-to-point tracks
     * use -1. This forced 36 onto a POINT-TO-POINT track on the theory that -1
     * was why a generated track rendered against a flat clear colour -- but that
     * was written [UNCERTAIN] and untested, and the missing FORWSKY.png it names
     * as the other candidate has since been fixed (tg_install_sky copies one in,
     * and the renderer now logs it loading at 256x256, probe class=SUNNY).
     *
     * Being chased 2026-08-26: the auto track shows a large dark region overhead
     * that is NOT geometry (survives every terrain/tunnel knob) and NOT weather
     * (LEVELINF says none, runtime confirms particles=0). The sky draws with
     * z_func=ALWAYS/z_write=0, so anywhere it fails to cover you see the clear
     * colour -- which is what that dark region looks like. Knob so the value can
     * be swept without a rebuild; -1 is the shipped P2P value.
     *
     * RESULT: swept to -1, frame IDENTICAL -- this field is NOT the cause, so
     * the default stays at the value that has actually been driven. The next
     * suspect is the fog/background COLOUR written at 0x60..0x62 just below,
     * which this generator leaves at 0,0,0: if the renderer clears to the
     * level's background colour, every part of the view the sky band does not
     * cover is filled with BLACK, which is exactly what the dark region is. */
    tg_put_u32(out, (unsigned)td5_env_int("TD5RE_AUTOTRACK_SKY_ANIM",
                                          36, -1, 64));
    tg_put_u32(out, (unsigned)nspans);          /* 0x58 total_span_count */
    tg_put_u32(out, 0);                         /* 0x5C fog_enabled */
    tg_put_u8(out, 0);                          /* 0x60 fog r */
    tg_put_u8(out, 0);                          /* 0x61 fog g */
    tg_put_u8(out, 0);                          /* 0x62 fog b */
    tg_put_u8(out, 0);                          /* 0x63 pad */
    return !out->oom;
}

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
#define TD5_TG_SPANS_PER_ENTRY 4     /* entry = span >> 2 */

static void tg_put_f32(TG_Buf *buf, double v)
{
    float f = (float)v;
    unsigned int u;
    memcpy(&u, &f, sizeof(u));
    tg_put_u32(buf, u);
}

/* Left and right road edge at fraction f along span si. */
static void tg_road_edge(const TG_NodeList *nl, int si, double f, double shift,
                         double wscale,
                         double *lx, double *ly, double *lz,
                         double *rx, double *ry, double *rz)
{
    const TG_Node *a = &nl->v[si];
    const TG_Node *b = &nl->v[si + 1];
    double x = a->x + (b->x - a->x) * f;
    double y = a->y + (b->y - a->y) * f;
    double z = a->z + (b->z - a->z) * f;
    double w = (a->width + (b->width - a->width) * f) * wscale;
    double tx = a->tx + (b->tx - a->tx) * f;
    double tz = a->tz + (b->tz - a->tz) * f;
    double m = sqrt(tx * tx + tz * tz);
    if (m < 1e-9) { tx = 0.0; tz = 1.0; m = 1.0; }
    tx /= m; tz /= m;
    /* Left of travel is (tz, -tx), matching the strip row order. `shift` slides
     * the whole cross-section along that axis and `wscale` scales its width --
     * 1.0/0 for the plain main road, 0.5 + a half-width offset for the split
     * fork carriageways, so the road mesh follows the strip rows exactly. */
    x += tz * shift; z -= tx * shift;
    *lx = x + tz * (w * 0.5); *ly = y; *lz = z - tx * (w * 0.5);
    *rx = x - tz * (w * 0.5); *ry = y; *rz = z + tx * (w * 0.5);
}

/* One road mesh for span si, laterally offset by shift_near..shift_far AND
 * width-scaled wscale_near..wscale_far across the span. Appended to blk.
 *
 * The width TAPERS across the span rather than being one number for the whole
 * of it (2026-08-27): a branch corridor widens continuously (tg_branch_wscale),
 * and a constant-per-span width turns that taper back into the staircase the
 * strip rows no longer have -- the mesh would then disagree with the surface
 * you collide with. tg_emit_road_quad below passes the same value twice, so the
 * plain road and the fixed-width halves are unchanged. Returns 0 on OOM.
 *
 * ITEMS 7 & 11 (2026-08-28): the across-road U is `u_scale * wscale`, NOT a flat
 * lane count anchored edge-to-edge. Root cause of "textures moving on branches"
 * and "lane markers shift when adding lanes": the old code set the right-edge U
 * to a per-span integer lane count while the physical width tapered
 * continuously (tg_branch_wscale) and stepped the lane count independently. That
 * made the world-space tile size vary along a corridor (the asphalt "moved") and
 * jump at each lane-count change (the lane paint "shifted"), because on the main
 * ring width and lane count are both constant so the bug never showed. Tying U
 * to wscale makes the tile size (physical_width / U = width/u_scale) CONSTANT in
 * world units along the whole corridor and continuous across span joins: with a
 * per-corridor-constant u_scale, adjacent spans agree at their shared node
 * because U there depends only on the shared wscale, not on either span's lane
 * count. Constant-width callers (main road wscale 1.0, fork half wscale 0.5)
 * pass u_scale = lanes/wscale via tg_emit_road_quad, so their U is byte-identical
 * to before. `u_scale` = the U reached at the right edge when wscale == 1.0. */
static int tg_emit_road_quad_taper(const TG_NodeList *nl, int si, double u_scale,
                                   double shift_near, double shift_far,
                                   double wscale_near, double wscale_far,
                                   int page, TG_Buf *blk)
{
    double px[TD5_TG_ROAD_SUBDIV * 4], py[TD5_TG_ROAD_SUBDIV * 4];
    double pz[TD5_TG_ROAD_SUBDIV * 4], uu[TD5_TG_ROAD_SUBDIV * 4];
    double vv[TD5_TG_ROAD_SUBDIV * 4];
    double cx = 0.0, cy = 0.0, cz = 0.0, radius = 0.0;
    int k, i, n = 0;

    for (k = 0; k < TD5_TG_ROAD_SUBDIV; k++) {
        double f0 = (double)k / (double)TD5_TG_ROAD_SUBDIV;
        double f1 = (double)(k + 1) / (double)TD5_TG_ROAD_SUBDIV;
        double nlx, nly, nlz, nrx, nry, nrz;
        double flx, fly, flz, frx, fry, frz;
        double s0v = shift_near + (shift_far - shift_near) * f0;
        double s1v = shift_near + (shift_far - shift_near) * f1;
        double w0v = wscale_near + (wscale_far - wscale_near) * f0;
        double w1v = wscale_near + (wscale_far - wscale_near) * f1;
        tg_road_edge(nl, si, f0, s0v, w0v, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
        tg_road_edge(nl, si, f1, s1v, w1v, &flx, &fly, &flz, &frx, &fry, &frz);
        /* Right-edge U tracks the PHYSICAL width at each subdiv end (u_scale *
         * wscale), so the lane paint keeps a constant world-space pitch along a
         * tapering/widening corridor instead of stretching and jumping. */
        {
        const double ur0 = u_scale * w0v;
        const double ur1 = u_scale * w1v;
        /* Quad loop: near-left, near-right, far-right, far-left. */
        px[n]=nlx; py[n]=nly; pz[n]=nlz; uu[n]=0.0; vv[n]=si+f0; n++;
        px[n]=nrx; py[n]=nry; pz[n]=nrz; uu[n]=ur0; vv[n]=si+f0; n++;
        px[n]=frx; py[n]=fry; pz[n]=frz; uu[n]=ur1; vv[n]=si+f1; n++;
        px[n]=flx; py[n]=fly; pz[n]=flz; uu[n]=0.0; vv[n]=si+f1; n++;
        }
    }

    for (i = 0; i < n; i++) { cx += px[i]; cy += py[i]; cz += pz[i]; }
    cx /= n; cy /= n; cz /= n;
    for (i = 0; i < n; i++) {
        double dx = px[i]-cx, dy = py[i]-cy, dz = pz[i]-cz;
        double d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d > radius) radius = d;
    }
    if (!(radius > 0.0)) radius = 1.0;   /* NaN/<=0 is rejected by the culler */

    /* --- mesh record (0x38) --- */
    tg_put_u16(blk, 259);            /* 0x00 render_type (nothing reads it) */
    tg_put_u16(blk, 0);              /* 0x02 billboard tag: 0 = opaque */
    tg_put_u32(blk, 1);              /* 0x04 command_count */
    tg_put_u32(blk, (unsigned)n);    /* 0x08 total_vertex_count */
    tg_put_f32(blk, radius);         /* 0x0C bounding_radius */
    tg_put_f32(blk, cx);             /* 0x10 bounding_center */
    tg_put_f32(blk, cy);
    tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0);            /* 0x1C origin: 0 for opaque geometry */
    tg_put_f32(blk, 0.0);
    tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);              /* 0x28 reserved */
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);                       /* commands */
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);     /* vertices */
    tg_put_u32(blk, 0);              /* 0x34 normals: NULL is allowed */

    /* --- one command: quads only, sequential vertex cursor --- */
    tg_put_u16(blk, 0);              /* dispatch_type 0 = TRISTRIP */
    tg_put_u16(blk, (unsigned)page); /* texture_page_id: the SAMPLED page */
    tg_put_u32(blk, 0);              /* reserved */
    tg_put_u16(blk, 0);                             /* triangle_count */
    tg_put_u16(blk, TD5_TG_ROAD_SUBDIV);            /* quad_count */
    tg_put_u32(blk, 0);              /* vertex_data_ptr 0 = sequential */

    /* --- de-indexed vertices, 44 B each --- */
    for (i = 0; i < n; i++) {
        tg_put_f32(blk, px[i]);
        tg_put_f32(blk, py[i]);
        tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0);        /* view xyz: filled at runtime */
        tg_put_f32(blk, 0.0);
        tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);/* lighting ARGB: full bright */
        tg_put_f32(blk, uu[i]);      /* UVs are normalised floats; >1 tiles */
        tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0);        /* proj_u/proj_v: runtime */
        tg_put_f32(blk, 0.0);
    }
    /* Every drivable quad lands here -- the plain road, the fork half-road and
     * the branch corridor all route through this one writer. */
    tg_acct(TG_ACCT_ROAD, si);
    return !blk->oom;
}

/* Constant-width road mesh: the taper emitter with one width for both ends.
 * Kept as its own name so every existing caller is untouched. */
static int tg_emit_road_quad(const TG_NodeList *nl, int si, int lanes,
                             double shift_near, double shift_far, double wscale,
                             int page, TG_Buf *blk)
{
    /* Constant width, so u_scale*wscale must reproduce the flat `lanes` the old
     * code wrote at the right edge. wscale is 0.5 or 1.0 here (exact in binary),
     * so lanes/wscale then *wscale is byte-identical to the old integer U. */
    const double u_scale = (wscale > 0.0) ? (double)lanes / wscale : (double)lanes;
    return tg_emit_road_quad_taper(nl, si, u_scale, shift_near, shift_far,
                                   wscale, wscale, page, blk);
}

/* Plain main-road mesh for span si (no lateral offset). */
static int tg_emit_road_mesh(const TG_NodeList *nl, int si, int lanes,
                             TG_Buf *blk)
{
    return tg_emit_road_quad(nl, si, lanes, 0.0, 0.0, 1.0, tg_road_page(si), blk);
}

/* Six quads. Corner sign triples per face, then which half-extents span the
 * face (for UV scaling). Winding is effectively free -- clip_and_submit_polygon
 * culls by screen area, and scenery is submitted CULL_NONE. */
static const signed char k_box_corner[6][4][3] = {
    {{-1, 1,-1},{ 1, 1,-1},{ 1, 1, 1},{-1, 1, 1}},   /* +Y top    */
    {{-1,-1, 1},{ 1,-1, 1},{ 1,-1,-1},{-1,-1,-1}},   /* -Y bottom */
    {{-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1}},   /* -Z        */
    {{ 1,-1, 1},{-1,-1, 1},{-1, 1, 1},{ 1, 1, 1}},   /* +Z        */
    {{-1,-1, 1},{-1,-1,-1},{-1, 1,-1},{-1, 1, 1}},   /* -X        */
    {{ 1,-1,-1},{ 1,-1, 1},{ 1, 1, 1},{ 1, 1,-1}}    /* +X        */
};
/* Half-extent axis pairs giving each face's (u,v) size: 0=x 1=y 2=z. */
static const signed char k_box_uv_axis[6][2] = {
    {0,2},{0,2},{0,1},{0,1},{2,1},{2,1}
};

/* A box (building / bridge pier / tunnel wall) centred at c with half-extents
 * h, textured from `page`, tiling every `tile` world units. */
static int tg_emit_box_mesh(TG_Buf *blk, double cx, double cy, double cz,
                            double hx, double hy, double hz,
                            double fx, double fz, int page, double tile,
                            unsigned int color)
{
    /* Box frame: fwd = (fx,fz) along the road, right = (fz,-fx), up = +Y.
     * An axis-aligned box is fine for a building but wrong for anything that
     * must FOLLOW a curving road (tunnel walls, bridge decks), so hz runs
     * along the road and hx across it. */
    const double rx = fz, rz = -fx;
    const double hv[3] = { hx, hy, hz };
    double radius = sqrt(hx*hx + hy*hy + hz*hz);
    int f, i;

    if (!(radius > 0.0)) radius = 1.0;
    if (tile <= 0.0) tile = 1500.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 0);                    /* opaque, not a billboard */
    tg_put_u32(blk, 1);                    /* one command */
    tg_put_u32(blk, 6 * 4);                /* 6 quads, de-indexed */
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx);
    tg_put_f32(blk, cy);
    tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    tg_put_u16(blk, 0);                    /* dispatch_type 0 */
    tg_put_u16(blk, (unsigned)page);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);                    /* triangle_count */
    tg_put_u16(blk, 6);                    /* quad_count */
    tg_put_u32(blk, 0);

    for (f = 0; f < 6; f++) {
        double ua = 2.0 * hv[(int)k_box_uv_axis[f][0]] / tile;
        double vb = 2.0 * hv[(int)k_box_uv_axis[f][1]] / tile;
        for (i = 0; i < 4; i++) {
            const double sx = k_box_corner[f][i][0];
            const double sy = k_box_corner[f][i][1];
            const double sz = k_box_corner[f][i][2];
            tg_put_f32(blk, cx + rx * (sx * hx) + fx * (sz * hz));
            tg_put_f32(blk, cy +       sy * hy);
            tg_put_f32(blk, cz + rz * (sx * hx) + fz * (sz * hz));
            tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
            tg_put_u32(blk, color);
            /* Corner order walks the quad loop, so (0,0)(u,0)(u,v)(0,v). */
            tg_put_f32(blk, (i == 1 || i == 2) ? ua : 0.0);
            tg_put_f32(blk, (i >= 2) ? vb : 0.0);
            tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        }
    }
    return !blk->oom;
}

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
static int tg_tree_page_is_half(int page);

static int tg_emit_billboard_mesh(TG_Buf *blk, double wx, double wy, double wz,
                                  double half_w, double height, int page, int tag)
{
    double radius = sqrt(half_w * half_w + height * height);
    /* A HALF page is drawn as TWO quads meeting at the billboard's vertical
     * axis, the second with u reversed -- mirror-and-duplicate, which is how
     * the shipped level placed the pair. Whole pages stay one quad. */
    const int nq = tg_tree_page_is_half(page) ? 2 : 1;
    int q, i;
    static const double ly[4] = {  0.0,  0.0,  1.0,  1.0 };

    if (!(radius > 0.0)) radius = 1.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, (unsigned)tag);        /* 1 = camera-facing, 2 = additive */
    tg_put_u32(blk, 1);                    /* one command */
    tg_put_u32(blk, (unsigned)(4 * nq));   /* one quad, or two when mirrored */
    tg_put_f32(blk, radius);
    tg_put_f32(blk, wx);                   /* bounding centre stays world */
    tg_put_f32(blk, wy + height * 0.5);
    tg_put_f32(blk, wz);
    tg_put_f32(blk, wx * 256.0);           /* origin is 24.8 for billboards */
    tg_put_f32(blk, wy * 256.0);
    tg_put_f32(blk, wz * 256.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    tg_put_u16(blk, 0);                    /* dispatch_type 0 */
    tg_put_u16(blk, (unsigned)page);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);
    tg_put_u16(blk, (unsigned)nq);         /* quad_count */
    tg_put_u32(blk, 0);

    for (q = 0; q < nq; q++) {
        /* Whole page: one quad spanning -half_w..+half_w, u 0..1.
         * Half page: the image's own axis is its u=1 edge (measured: every
         * half page's foliage is FLUSH at column 63 with a keyed left margin),
         * so quad 0 runs -half_w..0 with u 0..1 and quad 1 runs 0..+half_w
         * with u 1..0 -- the seam is the axis and the halves match exactly. */
        const double x0 = (nq == 1 || q == 0) ? -half_w : 0.0;
        const double x1 = (nq == 1 || q == 1) ?  half_w : 0.0;
        const double u0 = (q == 1) ? 1.0 : 0.0;
        const double u1 = (q == 1) ? 0.0 : 1.0;
        for (i = 0; i < 4; i++) {
            /* local: x across, y up. Quad loop order is near-bottom,
             * far-bottom, far-top, near-top, so 1 and 2 take the far edge. */
            tg_put_f32(blk, (i == 1 || i == 2) ? x1 : x0);
            tg_put_f32(blk, ly[i] * height);
            tg_put_f32(blk, 0.0);
            tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
            tg_put_u32(blk, 0xFFFFFFFFu);
            tg_put_f32(blk, (i == 1 || i == 2) ? u1 : u0);
            tg_put_f32(blk, (i >= 2) ? 0.0 : 1.0);   /* v flipped: base at v=1 */
            tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        }
    }
    return !blk->oom;
}

/* ===================== FACADE WALLS =====================
 * Shipped TD5 buildings are NOT closed boxes with a UV-wrapped masonry texture.
 * A survey of level014 (Melbourne, 1743 meshes) found exactly ONE 6-quad box:
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

/* Write ONE opaque quad-list mesh in the 0x38 format, split into up to `nseg`
 * COMMANDS -- each command samples its own page over its own run of quads, in
 * vertex order. That is how one building can carry a shop page on the ground
 * floor and a wall page above without a second mesh (which would corrupt the
 * one-mesh-per-building offset accounting in tg_emit_models). */
static int tg_write_quad_mesh(TG_Buf *blk, const double *px, const double *py,
                              const double *pz, const double *uu, const double *vv,
                              int n, const int *seg_page, const int *seg_nq,
                              int nseg)
{
    double cx = 0.0, cy = 0.0, cz = 0.0, radius = 0.0;
    int i, s;

    if (n <= 0 || nseg <= 0) return 1;
    for (i = 0; i < n; i++) { cx += px[i]; cy += py[i]; cz += pz[i]; }
    cx /= n; cy /= n; cz /= n;
    for (i = 0; i < n; i++) {
        double dx = px[i]-cx, dy = py[i]-cy, dz = pz[i]-cz;
        double d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d > radius) radius = d;
    }
    if (!(radius > 0.0)) radius = 1.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 0);                    /* opaque, not a billboard */
    tg_put_u32(blk, (unsigned)nseg);       /* command_count */
    tg_put_u32(blk, (unsigned)n);          /* total de-indexed vertices */
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx);
    tg_put_f32(blk, cy);
    tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + nseg * TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    for (s = 0; s < nseg; s++) {
        tg_put_u16(blk, 0);                    /* dispatch_type 0 */
        tg_put_u16(blk, (unsigned)seg_page[s]);
        tg_put_u32(blk, 0);
        tg_put_u16(blk, 0);                    /* triangle_count */
        tg_put_u16(blk, (unsigned)seg_nq[s]);  /* quad_count */
        tg_put_u32(blk, 0);
    }

    for (i = 0; i < n; i++) {
        tg_put_f32(blk, px[i]);
        tg_put_f32(blk, py[i]);
        tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);
        tg_put_f32(blk, uu[i]);
        tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    return !blk->oom;
}

/* [R5 STRUCT item 9] Same as tg_write_quad_mesh but with a PER-VERTEX colour,
 * so one mesh can carry both the DIM tunnel walls and a BRIGHT portal lintel
 * (the swept tunnel keeps everything in a single mesh per span so the caller's
 * offset bookkeeping records exactly one model). */
static int tg_write_quad_mesh_col(TG_Buf *blk, const double *px, const double *py,
                                  const double *pz, const double *uu, const double *vv,
                                  const unsigned int *col, int n,
                                  const int *seg_page, const int *seg_nq, int nseg)
{
    double cx = 0.0, cy = 0.0, cz = 0.0, radius = 0.0;
    int i, s;

    if (n <= 0 || nseg <= 0) return 1;
    for (i = 0; i < n; i++) { cx += px[i]; cy += py[i]; cz += pz[i]; }
    cx /= n; cy /= n; cz /= n;
    for (i = 0; i < n; i++) {
        double dx = px[i]-cx, dy = py[i]-cy, dz = pz[i]-cz;
        double d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d > radius) radius = d;
    }
    if (!(radius > 0.0)) radius = 1.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 0);                    /* opaque, not a billboard */
    tg_put_u32(blk, (unsigned)nseg);
    tg_put_u32(blk, (unsigned)n);
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx);
    tg_put_f32(blk, cy);
    tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + nseg * TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    for (s = 0; s < nseg; s++) {
        tg_put_u16(blk, 0);
        tg_put_u16(blk, (unsigned)seg_page[s]);
        tg_put_u32(blk, 0);
        tg_put_u16(blk, 0);
        tg_put_u16(blk, (unsigned)seg_nq[s]);
        tg_put_u32(blk, 0);
    }

    for (i = 0; i < n; i++) {
        tg_put_f32(blk, px[i]);
        tg_put_f32(blk, py[i]);
        tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, col[i]);
        tg_put_f32(blk, uu[i]);
        tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    return !blk->oom;
}

/* Push floors [r0,r1) of a cols x `rows` grid onto the vertex arrays. `base` is
 * the lower-near corner; `across` and `up` are the FULL edge vectors (up spans
 * all `rows` floors). Each cell maps the whole page (UV 0..1, v=1 at the base),
 * so no image is cut mid-cell. Splitting the row range lets the ground floor go
 * on a shop page and the floors above on the wall page. */
static void tg_facade_push_grid(double bx, double by, double bz,
                                double ax, double ay, double az,
                                double ux, double uy, double uz,
                                int cols, int rows, int r0, int r1,
                                double *px, double *py, double *pz,
                                double *uu, double *vv, int *pn)
{
    const double e0 = TD5_TG_FACADE_UV_INSET, e1 = 1.0 - TD5_TG_FACADE_UV_INSET;
    int c, r, n = *pn;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (r0 < 0) r0 = 0;
    if (r1 > rows) r1 = rows;
    for (r = r0; r < r1; r++) {
        for (c = 0; c < cols; c++) {
            double c0 = (double)c / cols, c1 = (double)(c + 1) / cols;
            double r0f = (double)r / rows, r1f = (double)(r + 1) / rows;
            double rr0 = r0f, rr1 = r1f;
            if (n + 4 > TD5_TG_FACADE_MAXQUAD * 4) { *pn = n; return; }
            /* quad loop: near-bottom, far-bottom, far-top, near-top */
            px[n]=bx+ax*c0+ux*rr0; py[n]=by+ay*c0+uy*rr0; pz[n]=bz+az*c0+uz*rr0; uu[n]=e0; vv[n]=e1; n++;
            px[n]=bx+ax*c1+ux*rr0; py[n]=by+ay*c1+uy*rr0; pz[n]=bz+az*c1+uz*rr0; uu[n]=e1; vv[n]=e1; n++;
            px[n]=bx+ax*c1+ux*rr1; py[n]=by+ay*c1+uy*rr1; pz[n]=bz+az*c1+uz*rr1; uu[n]=e1; vv[n]=e0; n++;
            px[n]=bx+ax*c0+ux*rr1; py[n]=by+ay*c0+uy*rr1; pz[n]=bz+az*c0+uz*rr1; uu[n]=e0; vv[n]=e0; n++;
        }
    }
    *pn = n;
}

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

/* Resolve span si on one side to its superblock, its phase within it, that
 * block's side street (start phase + length) and whether it is an avenue. */
static void tg_facade_block(int si, int left, unsigned int *block,
                            unsigned int *phase, unsigned int *gs,
                            unsigned int *gl, int *avenue)
{
    const unsigned int ab = (unsigned)si / TD5_TG_FACADE_PERIOD;
    const int av = (int)(((ab * 2654435761u) >> 28) % (unsigned)TD5_TG_AVENUE_IN
                         == 0u);
    const unsigned int s = (unsigned)si + ((!av && left) ? 777u : 0u);
    const unsigned int blk = s / TD5_TG_FACADE_PERIOD;
    const unsigned int h = blk * 2654435761u;

    *block  = blk;
    *phase  = s % TD5_TG_FACADE_PERIOD;
    *gs     = 2u + ((h >> 27) % 11u);            /* run before the street */
    /* An avenue is wider than a street, and that is the whole point of the
     * distinction: 6..8 spans (9000..12000 raw, six to eight lanes) against the
     * 2..4 of a minor street. */
    *gl     = av ? (6u + ((h >> 23) % 3u)) : (2u + ((h >> 23) % 3u));
    *avenue = av;
}

static int tg_facade_built(int si, int left)
{
    unsigned int block, phase, gs, gl;
    int av;

    /* Off the near end of the track there is nothing, so span 0 always gets a
     * corner return rather than a wall that starts as a bare edge. */
    if (si <= 0) return 0;
    /* "Start at the very beginnings with buildings": the opening stretch is
     * FORCED built on both sides. Otherwise the run/gap hash decides it, and
     * on most seeds it opens a gap right where the grid sits -- the start line
     * came up in bare ground even though span 0 is always the CITY biome. */
    if (si < TD5_TG_FACADE_START_RUN &&
        td5_env_flag_on("TD5RE_AUTOTRACK_START_CITY"))
        return 1;

    tg_facade_block(si, left, &block, &phase, &gs, &gl, &av);
    return (int)(phase < gs || phase >= gs + gl);
}

/* Does a facade actually STAND on side `left` (1=left,0=right) at span si? The
 * run/gap pattern (tg_facade_built) is not the whole story: a fork clears its
 * side<0 (=right, left==0) lateral for the branch corridor, and tg_side_geom
 * drops the wall there. Caps, step walls and neighbour-height queries must all
 * agree with tg_side_geom on this or a flank facing a suppressed side is left
 * open air -- the returning "some buildings still don't have sides, only a
 * facade" (item 15): a run whose FAR neighbour is a fork-cleared span had
 * cap_far == !tg_facade_built(si+1) == 0 (the pattern still says "built"), and
 * the step wall would even try to close to a building that was never emitted.
 * One predicate, shared, removes both mismatches. Gated so the widened coverage
 * can be A/B'd against the old cap-on-gap-only behaviour. */
static int tg_side_built(int si, int left)
{
    if (!tg_facade_built(si, left)) return 0;
    if (td5_env_flag_on("TD5RE_AUTOTRACK_SIDE_CLOSE") &&
        tg_branches_enabled() && !left && tg_span_in_fork_clear(si))
        return 0;
    /* [R5 item 15] tg_facade_built is the run/gap PATTERN only -- biome- and
     * bridge-blind. On a bridge deck (tg_building_for_span returns early) or in a
     * tree-billboard biome (COAST/FOREST/... build no wall) the pattern still
     * says "built", so a cap keyed off tg_side_built left the flank open where a
     * facade run met a bridge or a non-facade biome: "one span of the front of a
     * building disconnected from the following spans" (span 1047, the INDUSTRIAL
     * band 1040-1049 wedged between bridge run 1000-1039 and COAST 1050+). A span
     * where no wall actually stands must read as a run boundary so its neighbour
     * caps. Default ON; TD5RE_AUTOTRACK_EDGE_CAP=0 restores the pattern-only
     * predicate for an A/B. */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_EDGE_CAP") && !tg_facade_stands(si))
        return 0;
    return 1;
}

/* [R6 item 16] A facade span whose side stands but whose BOTH along-road
 * neighbours do NOT -- a building one span long, capped on both ends, floating
 * detached from anything. It is the biome BLEND dithering a single facade-biome
 * span into a run of tree-biome (COAST) spans: seed 99991 spans 1043 and 1047,
 * lone INDUSTRIAL blocks in the COAST band just past the bridge -- "1 span of
 * building not connected with anything ... if this is the transition, remove the
 * logic". So: do not build a lone one-span frontage. tg_side_built (not the raw
 * pattern) is used for the neighbours so a bridge/biome edge already counts as
 * "not built", and it does NOT itself call this, so there is no recursion.
 * Per-side, since a run can end isolated on one kerb while the other continues. */
static int tg_facade_isolated(int si, int left)
{
    if (!tg_side_built(si, left)) return 0;
    return !tg_side_built(si - 1, left) && !tg_side_built(si + 1, left);
}

/* Hash identifying the RUN span si belongs to on this side. A superblock now
 * holds up to TWO runs (before and after its side street), so keying pages and
 * floor counts on the superblock alone would give one texture and one height to
 * two buildings that are visibly separated by a street. */
static unsigned int tg_facade_run_id(int si, int left)
{
    unsigned int block, phase, gs, gl;
    int av;
    tg_facade_block(si, left, &block, &phase, &gs, &gl, &av);
    /* The side is part of the key: with avenues both kerbs share a block index,
     * so without it the two facing blocks would be one building repeated. */
    return ((block * 2u + (phase >= gs + gl ? 1u : 0u)) * 2u
            + (unsigned)(left ? 1 : 0)) * 2246822519u;
}

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

static int tg_city_district_floors(unsigned int block)
{
    double t, w;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_DISTRICTS")) return 0;
    t = (double)(block % (unsigned)TD5_TG_DISTRICT_BLOCKS)
      / (double)TD5_TG_DISTRICT_BLOCKS;
    w = 0.5 - 0.5 * cos(2.0 * 3.14159265358979 * t);
    return (int)(w * (double)TD5_TG_DOWNTOWN_FLOORS + 0.5);
}

/* Corner return at a run END, with real MASS. The first cut pushed a SINGLE
 * flat plane back along the lateral -- a zero-thickness sheet, which from any
 * oblique angle read as a folded piece of paper joined to the front wall: the
 * "two sides ... non width L shape" in the feedback. A run end now gets a
 * four-face prism (outer return, inner return one thickness into the run, the
 * rear face closing the back, and a roof), so the corner has body from every
 * angle and the block is capped when seen from a rise.
 *
 * `l` is the outward lateral unit, `ti` the along-road unit pointing INTO the
 * run. `cols` is how many whole page cells the return is deep (tg_facade_cap_cols
 * -- passing it in keeps the flank at the page's own aspect, like the front).
 * With thick == 0 the inner/rear/roof faces collapse to zero area, which the
 * screen-area cull drops -- that is the fallback when the knob is off. */
static void tg_facade_push_cap(double bx, double by, double bz,
                               double lx, double lz, double tix, double tiz,
                               double depth, double thick, double H, int rows,
                               int cols,
                               double *px, double *py, double *pz,
                               double *uu, double *vv, int *pn)
{
    const double dx = lx * depth, dz = lz * depth;
    const double tx = tix * thick, tz = tiz * thick;

    tg_facade_push_grid(bx, by, bz, dx, 0.0, dz, 0.0, H, 0.0,
                        cols, rows, 0, rows, px, py, pz, uu, vv, pn);
    tg_facade_push_grid(bx + tx, by, bz + tz, dx, 0.0, dz, 0.0, H, 0.0,
                        cols, rows, 0, rows, px, py, pz, uu, vv, pn);
    tg_facade_push_grid(bx + dx, by, bz + dz, tx, 0.0, tz, 0.0, H, 0.0,
                        1, rows, 0, rows, px, py, pz, uu, vv, pn);
    /* Roof: one horizontal cell, `up` reused as the along-road thickness. */
    tg_facade_push_grid(bx, by + H, bz, dx, 0.0, dz, tx, 0.0, tz,
                        cols, 1, 0, 1, px, py, pz, uu, vv, pn);
}

/* Push ONE quad from four explicit corners (near-bottom, far-bottom, far-top,
 * near-top order, like tg_facade_push_grid). Used where a face is not a
 * parallelogram -- a roof deck between a curved frontage and its back line --
 * so the corners can be taken from the geometry instead of a single lateral. */
static void tg_facade_push_quad(const double *xyz,
                                double *px, double *py, double *pz,
                                double *uu, double *vv, int *pn)
{
    static const double k_u[4] = { 0.0, 1.0, 1.0, 0.0 };
    static const double k_v[4] = { 1.0, 1.0, 0.0, 0.0 };
    const double e = TD5_TG_FACADE_UV_INSET;
    int i, n = *pn;
    if (n + 4 > TD5_TG_FACADE_MAXQUAD * 4) return;
    for (i = 0; i < 4; i++) {
        px[n] = xyz[i * 3 + 0];
        py[n] = xyz[i * 3 + 1];
        pz[n] = xyz[i * 3 + 2];
        uu[n] = k_u[i] > 0.5 ? 1.0 - e : e;
        vv[n] = k_v[i] > 0.5 ? 1.0 - e : e;
        n++;
    }
    *pn = n;
}

/* Which facade page a RUN uses -- keyed to the run hash so a whole building is
 * one texture but neighbouring buildings differ, the way a real street mixes
 * stone/glass/brick frontages. Variant 0 is the base WALL page; 1..N-1 are the
 * extra pages appended after GROUND.
 *
 * `rows` splits the palette by BUILDING CLASS, not at random: a run tall enough
 * to be a tower draws from the TOWER variants (office curtain wall) and anything
 * shorter from the low-rise masonry ones. Handing a 10-storey block a two-storey
 * shopfront texture is what makes a procedural city read as a texture soup
 * rather than a place. */
static int tg_facade_page_class(unsigned int gh, int rows)
{
    /* [R7 item 4] Draw from the UNION of the original 12 variants and the R7
     * variety pages, still split by class: a tower run picks among the original
     * tower variants (TOWER_FIRST..WALL_VARIANTS) plus the R7 tower pages, a
     * low-rise run among the original low variants (0..TOWER_FIRST) plus the R7
     * low pages. Same hash bits key the pick, so a given block keeps a stable
     * page. TD5RE_R7_CITY_VARIETY=0 restores the original 12-page pool for an
     * A/B. */
    const int r7 = td5_env_flag_on("TD5RE_R7_CITY_VARIETY");
    unsigned int v;
    if (rows >= TD5_TG_FACADE_TALL_ROWS) {
        const int orig  = TD5_TG_WALL_VARIANTS - TD5_TG_WALL_TOWER_FIRST; /* 3 */
        const int extra = r7 ? TD5_TG_R7_WALL_TOWER_N : 0;
        v = (gh >> 17) % (unsigned)(orig + extra);
        if ((int)v < orig)
            return TD5_TG_PAGE_WALL_EXTRA + TD5_TG_WALL_TOWER_FIRST + (int)v - 1;
        return TD5_TG_PAGE_R7_WALL_TOWER + ((int)v - orig);
    } else {
        const int orig  = TD5_TG_WALL_TOWER_FIRST;                        /* 9 */
        const int extra = r7 ? TD5_TG_R7_WALL_LOW_N : 0;
        v = (gh >> 17) % (unsigned)(orig + extra);
        if ((int)v < orig)
            return v == 0 ? TD5_TG_PAGE_WALL
                          : (TD5_TG_PAGE_WALL_EXTRA + (int)v - 1);
        return TD5_TG_PAGE_R7_WALL_LOW + ((int)v - orig);
    }
}

/* Which shop page a run's ground floor uses -- a different hash bit than the
 * wall page so the storefront and the tower above are chosen independently. */
static int tg_store_page(unsigned int gh)
{
    return TD5_TG_PAGE_STORE + (int)((gh >> 23) % (unsigned)TD5_TG_STORE_VARIANTS);
}

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
static const TG_TreePage k_tree_pages[TD5_TG_TREE_VARIANTS] = {
    { 4200, 5600, TG_TREE_DECID,   0 },  /* 0  L017 p266  deciduous         */
    { 5200, 6600, TG_TREE_DECID,   0 },  /* 1  L008 p173  big deciduous     */
    { 4000, 5400, TG_TREE_DECID,   1 },  /* 2  L013 p234  deciduous, half   */
    { 3200, 6400, TG_TREE_CONIFER, 1 },  /* 3  L003 p441  conifer, half     */
    { 3200, 6400, TG_TREE_CONIFER, 1 },  /* 4  L003 p445  snow conifer, half*/
    { 3400, 7200, TG_TREE_PALM,    0 },  /* 5  L026 p097  palm              */
    { 3800, 6800, TG_TREE_PALM,    0 },  /* 6  L014 p249  palm              */
    { 3600, 4200, TG_TREE_TOPIARY, 0 },  /* 7  L004 p359  topiary           */
    { 3600, 5000, TG_TREE_TOPIARY, 0 },  /* 8  L004 p360  topiary           */
    { 4200, 6000, TG_TREE_WILLOW,  1 }   /* 9  L004 p369  willow, half      */
};

/* TEXTURES.DAT page slot for tree variant v (variant 0 reuses PAGE_TREE). */
static int tg_tree_slot(int v)
{
    return v == 0 ? TD5_TG_PAGE_TREE : (TD5_TG_PAGE_TREE_EXTRA + v - 1);
}

/* [R5 item 18] Page slot for the v-th tall Moscow park tree, in the reserved
 * FLORA block. Only the first k_r5_flora_tree_count slots are ever filled. */
static int tg_flora_tree_slot(int v)
{
    return TD5_TG_PAGE_R5_FLORA + v;
}

/* [R7 item 8] Is FLORA page `v` a horizontally CONTINUOUS BAND rather than a
 * single-tree silhouette? A grove backdrop / treeline page is authored to TILE
 * edge-to-edge, so standing one upright as a camera-facing billboard shows a
 * SLICE of the band -- "the texture is cut off because it can be continuous".
 *
 * R6 fixed exactly this at span 462 but by a HAND-PICKED list { 0, 2 }: it
 * excluded FLORA index 1 (p412, the dense green grove wall) and KEPT index 2
 * (p424, the autumn/orange canopy). Index 2 has the SAME property -- it too
 * bleeds foliage off both edges -- so the user now reports the orange tree at the
 * same span. Listing pages fixes the instance; measuring the property fixes the
 * class (and any future page).
 *
 * The tell is in the alpha key (index 0 = transparent): a single tree leaves
 * transparent SKY down both sides, so its opaque pixels never reach both image
 * edges in the same row; a tiling band bleeds opaque foliage off BOTH edges so
 * neighbouring copies join. Measured over the shipped pages (64x64), the
 * both-edge-touch fraction is 0.00 for the individual canopy (FLORA0) and
 * 0.77 / 0.86 for the two bands (FLORA1 grove wall, FLORA2 autumn) -- a wide gap
 * the 0.40 threshold sits inside. */
static int tg_flora_page_is_band(int v)
{
    enum { PGW = 64, PGH = 64, KEY = 0, EDGE = 2 };
    const unsigned char *idx;
    int r, c, opaque = 0, both = 0;

    if (v < 0 || v >= k_r5_flora_tree_count) return 1;   /* unknown -> not upright */
    idx = k_r5_flora_tree_idx[v];
    for (r = 0; r < PGH; r++) {
        const unsigned char *row = idx + r * PGW;
        int lo = -1, hi = -1;
        for (c = 0; c < PGW; c++)
            if (row[c] != KEY) { if (lo < 0) lo = c; hi = c; }
        if (lo < 0) continue;                            /* fully transparent row */
        opaque++;
        if (lo <= EDGE && hi >= PGW - 1 - EDGE) both++;  /* reaches both edges */
    }
    if (opaque <= 0) return 1;
    return (both * 100) >= (opaque * 40);
}

/* [R6 item 7 / R7 item 8] Pick a FLORA page fit to stand upright as ONE tree.
 * Selects on the band PROPERTY (tg_flora_page_is_band) over the whole set rather
 * than a fixed index list, so every band page -- present or future -- is skipped.
 * Returns the FLORA index picked by `pick`, or -1 if no individual page exists. */
static int tg_flora_upright_index(unsigned int pick)
{
    int v, n = 0, avail[TD5_TG_R5_FLORA_N];

    for (v = 0; v < k_r5_flora_tree_count && v < TD5_TG_R5_FLORA_N; v++)
        if (!tg_flora_page_is_band(v)) avail[n++] = v;
    if (n <= 0) return -1;
    return avail[pick % (unsigned)n];
}

/* Inverse of tg_tree_slot: tree variant drawn on `page`, or -1 if it is not a
 * tree page at all. The billboard writer only has the page id to go on. */
static int tg_tree_variant_of_page(int page)
{
    if (page == TD5_TG_PAGE_TREE) return 0;
    if (page >= TD5_TG_PAGE_TREE_EXTRA &&
        page <  TD5_TG_PAGE_TREE_EXTRA + TD5_TG_TREE_VARIANTS - 1)
        return page - TD5_TG_PAGE_TREE_EXTRA + 1;
    return -1;
}

static int tg_real_textures_enabled(void);   /* defined with the page emitters */

/* Does `page` need mirror-and-duplicate? Only a real (borrowed) tree page that
 * the measurement above found to be one half of a pair. */
static int tg_tree_page_is_half(int page)
{
    const int v = tg_tree_variant_of_page(page);
    if (v < 0 || !k_tree_pages[v].half) return 0;
    if (!tg_real_textures_enabled()) return 0;   /* procedural pages are whole */
    /* Default ON (2026-08-26); TD5RE_AUTOTRACK_TREE_MIRROR=0 restores the raw
     * half page, i.e. the sliced-tree look that was reported. */
    return td5_env_flag_on("TD5RE_AUTOTRACK_TREE_MIRROR");
}

/* ---- branch-corridor clearance for verge scenery ----
 * Trees are placed off the MAIN RING only, so where a branch corridor runs
 * alongside they land on the branch carriageway. The corridor bows up to
 * TD5_TG_BRANCH_BOW (1.2) road widths into the -ve lateral, far past the
 * 800..3200 tree setback, which is why the trees were standing in the road.
 * Group E owns the fork data; this only READS it. */
#define TD5_TG_FLORA_BRANCH_MARGIN  600.0   /* verge left between road and trunk */

/* Push a verge setback (`gap`, measured from the road EDGE) out far enough that
 * nothing standing on it overlaps a branch carriageway. Returns `gap` unchanged
 * on the +ve lateral, which no corridor bows into.
 *
 * This used to carry its own tg_flora_branch_reach, one of the two hand-rolled
 * copies of the fork arithmetic the CARRIAGEWAY QUERY section was written to
 * retire. That copy assumed the corridor was a FIXED half carriageway
 * (width*0.25 out from its centre), which stopped being true once the corridor
 * learned to widen and taper, so it UNDER-REPORTED on a widened branch and
 * trees kept landing on it. Retired onto tg_carriageway_clear_gap at the
 * round-2 merge (2026-08-27).
 *
 * Off a fork the authority's reach floors at the main road's half width, so
 * `need` is just the margin -- 600, well under the 800 minimum tree setback and
 * the 11000 treeline setback, i.e. no change to placement away from a fork. */
static double tg_flora_gap_clear(const TG_NodeList *nl, int si, double side,
                                 double gap)
{
    if (side > 0.0) return gap;
    /* Default ON (2026-08-26); TD5RE_AUTOTRACK_FLORA_CLEAR=0 restores the old
     * placement, i.e. trees standing on the branch. */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_FLORA_CLEAR")) return gap;
    return tg_carriageway_clear_gap(nl, si, side, gap,
                                    TD5_TG_FLORA_BRANCH_MARGIN);
}

/* [R6 FLORA diag, dev-only] A camera-facing tree billboard is a DISC of radius
 * tw/2 about its plant point, so "clears the verge at span si" is not the same
 * as "clears the road": where the road CURVES back under a wide tree, or on a
 * fork, the tree can overhang a carriageway one or more spans away while its own
 * span's gap looks fine. This dumps, for a tree planted at world (cx,cz), the
 * SWEPT nearest approach of its disc to any road surface within +/-6 spans, as a
 * signed clearance (negative = the disc overlaps the road = spill). Read-only;
 * gated by TD5RE_FLORA_DIAG=1 and limited to spans near TD5RE_FLORA_DIAG_SPAN so
 * race.log stays legible. Confirms item 7/18 by number, not by eye. */
static void tg_flora_diag(const TG_NodeList *nl, int si, const char *kind,
                          int page, double side, double tw, double th,
                          double cx, double cz)
{
    int tgt, m, worst = -1;
    double min_clear = 1e18;
    if (!td5_env_flag_on("TD5RE_FLORA_DIAG")) return;
    tgt = td5_env_int("TD5RE_FLORA_DIAG_SPAN", 1413, 0, 100000);
    if (si < tgt - 8 || si > tgt + 8) return;
    for (m = si - 6; m <= si + 6; m++) {
        const TG_Node *nm, *nn;
        double dx, dz, lat, lon, seglen, clear;
        if (m < 0 || m + 1 >= nl->count) continue;
        nm  = &nl->v[m];
        nn  = &nl->v[m + 1];
        dx  = cx - nm->x; dz = cz - nm->z;
        lon = dx * nm->tx + dz * nm->tz;          /* along the span's tangent */
        seglen = sqrt((nn->x - nm->x) * (nn->x - nm->x) +
                      (nn->z - nm->z) * (nn->z - nm->z));
        /* only spans the tree is actually BESIDE (not a far stretch the road
         * doubles back near) -- a slab of one span length, small end slop. */
        if (lon < -600.0 || lon > seglen + 600.0) continue;
        lat = dx * nm->tz - dz * nm->tx;          /* signed lateral offset */
        if (lat < 0.0) lat = -lat;
        clear = lat - nm->width * 0.5 - tw * 0.5;  /* disc edge to road edge */
        if (clear < min_clear) { min_clear = clear; worst = m; }
    }
    TD5_LOG_I(LOG_TAG,
        "flora-diag: si=%d %s page=%d side=%+.0f tw=%.0f th=%.0f "
        "reach=%.0f swept_clear=%.0f @m=%d %s",
        si, kind, page, side, tw, th,
        tg_carriageway_reach(nl, si, side), min_clear, worst,
        (min_clear < 0.0) ? "SPILL" : "ok");
}

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
static const TG_PropPage k_prop_pages[TD5_TG_PROP_COUNT] = {
    {  300,  700, 1, 1,    0, TG_PROP_PERSON },  /* 0 L001 p316 spectator   */
    {  300,  700, 1, 1,    0, TG_PROP_PERSON },  /* 1 L001 p318 spectator   */
    { 1400, 1700, 1, 1,    0, TG_PROP_STATUE },  /* 2 L004 p446 lion        */
    { 1800, 4200, 1, 1,    0, TG_PROP_STATUE },  /* 3 L014 p274 monument    */
    { 1300, 1000, 1, 1,    0, TG_PROP_ANIMAL },  /* 4 L001 p341 sheep       */
    { 1300, 1600, 1, 1,    0, TG_PROP_ANIMAL },  /* 5 L003 p453 deer        */
    {  800,  800, 2, 3, 2500, TG_PROP_LAMP   }   /* 6 L001 p378 lamp glow   */
};
static int tg_prop_slot(int i) { return TD5_TG_PAGE_PROP + i; }

/* ===================== ROAD SURFACES =====================
 * Each biome drives on one surface: a GRIP class written into the strip's
 * surface byte (so the car really slides on ice / drags on cobble) plus a
 * matching texture. Grip classes are the shipped values (td5_physics.c grip
 * table): 1 tarmac 1.0, 4 gravel 0.98, 3 dirt 0.94, 5 cobble 0.75, 6 ice 0.70. */
enum { RS_TARMAC = 0, RS_GRAVEL, RS_DIRT, RS_ICE, RS_COBBLE };
typedef struct { int grip_class; int page_var; int proc_kind; } TG_RoadSurf;
static const TG_RoadSurf k_road_surf[TD5_TG_ROAD_VARIANTS] = {
    { 1, 0, RS_TARMAC },   /* dry asphalt, full grip (base ROAD page) */
    { 4, 1, RS_GRAVEL },   /* packed gravel */
    { 3, 2, RS_DIRT   },   /* dirt */
    { 6, 3, RS_ICE    },   /* ice / snow -- slippery */
    { 5, 4, RS_COBBLE }    /* cobble -- draggy stone */
};
static int tg_road_slot(int v)
{
    return v == 0 ? TD5_TG_PAGE_ROAD : (TD5_TG_PAGE_ROAD_EXTRA + v - 1);
}

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

/*                                                clim urb  brdg tunl rpt */
static const TG_Biome k_biomes[] = {
    /* CITY: ~8.4x11.5 wu cells, ~3 floors, on the curb, big sparse towers.
     * Few bridges (a city road crosses at grade), the odd underpass. */
    { "CITY",       9, 2150, 2950, 2, 3, 6000, 350, {0}, 0,
      TD5_TG_PAGE_WALL,   3, 0, TD5_TG_PAGE_GROUND,   6, 1, PP_MONUMENT, -1, 0, RS_TARMAC,
      1, 3,  40,   0, 3 },
    /* FIELDS: sparse deciduous on an open horizon; grazing sheep; dirt road.
     * COUNTRYSIDE = BRIDGES: open farmland is where the road crosses rivers and
     * dry valleys, which is the user's "if it's a countryside there should be
     * more bridges". Nothing to tunnel through. */
    { "FIELDS",     2, 0,0,0,0,0,0, {2, 0},       2,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN,   0, 0, -1, PP_SHEEP, 0, RS_DIRT,
      1, 1, 190,  10, 2 },
    /* FOREST: dense mixed deciduous crowding the verge; deer. */
    { "FOREST",    11, 0,0,0,0,0,0, {0, 1, 2},    3,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN,   0, 0, -1, PP_DEER, 0, RS_TARMAC,
      1, 0, 110,  70, 2 },
    /* INDUSTRIAL: wider squat sheds; gravel yards. The transition biome between
     * a city and open country -- see the adjacency reasoning below. */
    { "INDUSTRIAL", 6, 2560, 2300, 1, 2, 8000, 600, {0}, 0,
      TD5_TG_PAGE_WALL,  63, 0, TD5_TG_PAGE_GROUND,   3, 1, -1, -1, 0, RS_GRAVEL,
      1, 2,  90,  40, 2 },
    /* ALPINE: conifers + snow conifers; deer; icy road.
     * MOUNTAINS = TUNNELS, the user's second example. Also bridges (viaducts
     * across the valleys between the bores), but tunnels dominate. */
    { "ALPINE",     8, 0,0,0,0,0,0, {3, 4},       2,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN,   0, 0, -1, PP_DEER, 0, RS_ICE,
      2, 0, 140, 230, 2 },
    /* COAST: palms; beach crowds + promenade lamps; the sea alongside.
     * Causeways over inlets, headland bores. repeat_max 1 ON PURPOSE: the water
     * emitter keys its sea level and its seaward side off the 150-span cell, so
     * a COAST spanning two cells could step the sea surface or flip which side
     * it is on. Single-cell coasts keep that emitter exactly as correct as it is
     * today. Lift this only together with tg_biome_run_bounds adoption there. */
    { "COAST",      5, 0,0,0,0,0,0, {5, 6},       2,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN,   6, 1, -1, -1, 1, RS_TARMAC,
      0, 1, 160,  60, 1 },
    /* ORIENTAL: manicured topiary + weeping willow; guardian lions; cobbles. */
    { "ORIENTAL",   9, 0,0,0,0,0,0, {7, 8, 9},    3,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN,   4, 0, PP_LION, -1, 0, RS_COBBLE,
      1, 2, 120,  50, 2 }
};
#define TD5_TG_BIOME_COUNT 7
#define TD5_TG_BIOME_RUN   150

/* ==========================================================================
 * BIOME ADJACENCY  (feedback R2 item 23)
 *
 * THE COMPLAINT was that biome transitions are abrupt and that the biome does
 * not change what gets built. Both come from the same line of code: the biome
 * used to be a raw hash of (span / 150), so consecutive runs were INDEPENDENT
 * draws from all 7 biomes with a hard cut at the boundary. That produces snow
 * conifers ending and palm trees starting on the same span, which is the
 * "incompatibility due to difference" the user asked to be reasoned about.
 *
 * HOW LONG IS A RUN? A span is TD5_TG_SPAN_LENGTH (1500) world units and a lane
 * is TD5_TG_LANE_WIDTH (1500) world units. Taking a lane as a real 3.5 m lane,
 * one span is ~3.5 m and a 150-span run is ~525 m of road. That number is what
 * makes the rules below obvious rather than arbitrary: half a kilometre.
 *
 * RULE 1 -- CLIMATE, |climate[a] - climate[b]| <= 1.
 *   You do not drive out of snowbound conifers into a palm-lined beach road in
 *   half a kilometre. Nothing in the world does that; a mountain road reaches
 *   the sea through a treeline, then foothills, then the coast. Adjacent runs
 *   must be at most one step apart on warm(0) / temperate(1) / cold(2), so
 *   ALPINE-COAST is rejected outright and can only occur with a temperate run
 *   (FIELDS, FOREST) between them.
 *   This rule also does the grip work for free: RS_ICE lives only in ALPINE and
 *   ALPINE can only touch temperate biomes, so the car never steps from cobbles
 *   straight onto ice at speed.
 *
 * RULE 2 -- URBANITY, |urbanity[a] - urbanity[b]| <= 1.
 *   Real roads step into a city, they do not cut into it: wilderness, farmland,
 *   the edge with its sheds and yards, then the dense centre. A CITY run whose
 *   neighbour is FOREST reads as two tracks spliced together. Requiring one step
 *   on wild(0) / rural(1) / edge(2) / urban(3) forces INDUSTRIAL or ORIENTAL to
 *   stand between CITY and open country, which is exactly the suburban belt the
 *   user is asking for -- it is not a separate feature, it falls out of the rule.
 *
 * RULE 3 -- REPETITION, up to repeat_max consecutive cells.
 *   The rules above are constraints; this is the one that lengthens things.
 *   A biome may hold several cells in a row, and the urban biomes are allowed
 *   the most (CITY 3 cells = ~1.5 km of city), which is the user's "urban and
 *   suburban areas longer". COAST is pinned to 1 for the emitter reason in its
 *   table row.
 *
 * The graph these rules leave is connected with no dead ends (every biome has at
 * least two legal successors), so the picker below can never get stuck and never
 * needs a "give up and take anything" escape that would reintroduce the bad cuts.
 * ========================================================================== */
static int tg_biome_compatible(int a, int b)
{
    int dc = k_biomes[a].climate  - k_biomes[b].climate;
    int du = k_biomes[a].urbanity - k_biomes[b].urbanity;
    if (dc < 0) dc = -dc;
    if (du < 0) du = -du;
    return dc <= 1 && du <= 1;
}

/* Biome per 150-span CELL. The cell grid is deliberately unchanged from the old
 * hash version: the water and prop emitters derive per-run state from
 * (span / TD5_TG_BIOME_RUN), so making runs variable-length would silently
 * desynchronise them. Length comes from repeating a cell instead. */
#define TD5_TG_BIOME_CELLS ((TD5_TG_MAX_SPANS / TD5_TG_BIOME_RUN) + 2)
static unsigned char s_biome_cell[TD5_TG_BIOME_CELLS];
static int           s_biome_laid_out = 0;

/* Deterministic per-cell hash. Not tg_rand(): the biome layout must not consume
 * the geometry RNG stream, or adding a biome rule would move the road. */
static unsigned int tg_biome_hash(unsigned int seed, int cell, unsigned int salt)
{
    unsigned int h = seed ^ (salt * 0x9E3779B9u);
    h += (unsigned)cell * 2654435761u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
}

/* Lay the whole cell grid out once per build, before any emitter asks. */
static void tg_biome_layout(unsigned int seed)
{
    int cell, run_len = 1;

    s_biome_cell[0] = (unsigned char)(tg_biome_hash(seed, 0, 1u)
                                      % TD5_TG_BIOME_COUNT);
    for (cell = 1; cell < TD5_TG_BIOME_CELLS; cell++) {
        int prev = s_biome_cell[cell - 1];
        unsigned int h = tg_biome_hash(seed, cell, 2u);
        int cand, chosen = -1, tries;

        /* Extend the current biome first, up to its repeat_max. The 0..99 draw
         * against a flat 45% keeps runs varied -- always extending to the cap
         * would make every city exactly repeat_max cells long. */
        if (run_len < k_biomes[prev].repeat_max && (h % 100u) < 45u) {
            s_biome_cell[cell] = (unsigned char)prev;
            run_len++;
            continue;
        }
        /* Otherwise walk the biome list from a hashed offset and take the first
         * COMPATIBLE one that is not the biome we just left (we already decided
         * not to extend it). Scanning from a rotating offset rather than
         * rejection-sampling keeps this bounded and deterministic. */
        for (tries = 0; tries < TD5_TG_BIOME_COUNT; tries++) {
            cand = (int)(((h >> 8) + (unsigned)tries) % TD5_TG_BIOME_COUNT);
            if (cand == prev) continue;
            if (!tg_biome_compatible(prev, cand)) continue;
            chosen = cand;
            break;
        }
        /* Unreachable by construction (the adjacency graph has no dead ends),
         * but a silent out-of-range index here would be a crash rather than a
         * cosmetic bug, so it is pinned rather than asserted. */
        if (chosen < 0) chosen = prev;
        s_biome_cell[cell] = (unsigned char)chosen;
        run_len = (chosen == prev) ? run_len + 1 : 1;
    }
    s_biome_laid_out = 1;
}

/* HARD cell assignment -- no blending. This is the biome that OWNS the span, and
 * it is what per-run state (road surface, sea level, run bounds) must key off:
 * a dithered road-surface class would flip grip from span to span. */
static int tg_biome_cell_index(int si)
{
    int cell;
    if (si < 0) si = 0;
    cell = si / TD5_TG_BIOME_RUN;
    if (cell >= TD5_TG_BIOME_CELLS) cell = TD5_TG_BIOME_CELLS - 1;
    if (!s_biome_laid_out)   /* asked before a build laid the grid out */
        return (int)(((unsigned)cell * 2654435761u) >> 27) % TD5_TG_BIOME_COUNT;
    return (int)s_biome_cell[cell];
}

/* Merged extent of the biome run containing si (consecutive cells of the same
 * biome collapse into one run). Offered for the emitters that currently do
 * `si / TD5_TG_BIOME_RUN` arithmetic to derive per-run state -- with repeated
 * cells that arithmetic sees a 300-span city as two runs. Not yet adopted by
 * them; see the COAST repeat_max note. */
static void tg_biome_run_bounds(int si, int *out_a, int *out_b)
{
    int cell = (si < 0) ? 0 : si / TD5_TG_BIOME_RUN;
    int b = tg_biome_cell_index(si), a = cell, z = cell;
    if (cell >= TD5_TG_BIOME_CELLS) cell = a = z = TD5_TG_BIOME_CELLS - 1;
    while (a > 0 && tg_biome_cell_index((a - 1) * TD5_TG_BIOME_RUN) == b) a--;
    while (z + 1 < TD5_TG_BIOME_CELLS &&
           tg_biome_cell_index((z + 1) * TD5_TG_BIOME_RUN) == b) z++;
    if (out_a) *out_a = a * TD5_TG_BIOME_RUN;
    if (out_b) *out_b = (z + 1) * TD5_TG_BIOME_RUN - 1;
}

/* Width of the dithered transition band, in spans, either side of a cell
 * boundary. 20 spans is ~70 m of road at the scale derived above: long enough
 * that the changeover reads as a gradient at racing speed, short enough that a
 * 150-span run still has a solid ~110-span core of its own character. */
#define TD5_TG_BIOME_BLEND 20

/* BLENDED biome for span si -- what the SCENERY emitters should ask.
 *
 * Inside the band around a cell boundary the span is assigned to the incoming or
 * the outgoing biome by a per-span hash whose threshold ramps linearly across
 * the band. That is ordered dithering of a categorical field, and it is the only
 * thing that actually smooths a transition between two DISCRETE biomes: you
 * cannot interpolate "forest" into "city", but you can interleave them, so the
 * trees thin out over ~140 m while the first buildings start appearing, instead
 * of every tree stopping and every building starting on one span.
 *
 * The hash is per-span and seed-independent-of-order, so the interleave is
 * stable across the thousands of calls an emitter makes for the same span. */
static int tg_biome_for_span(int si)
{
    int here, cell, off, other, dist;
    unsigned int h, thresh;

    if (si < 0) si = 0;
    here = tg_biome_cell_index(si);
    if (td5_env_int("TD5RE_AUTOTRACK_BIOME_BLEND", TD5_TG_BIOME_BLEND,
                    0, TD5_TG_BIOME_RUN / 2) <= 0)
        return here;

    cell = si / TD5_TG_BIOME_RUN;
    off  = si - cell * TD5_TG_BIOME_RUN;

    if (off < TD5_TG_BIOME_BLEND) {              /* leading edge of this cell */
        other = tg_biome_cell_index((cell - 1) * TD5_TG_BIOME_RUN);
        dist  = off;                              /* 0 = right on the boundary */
    } else if (off >= TD5_TG_BIOME_RUN - TD5_TG_BIOME_BLEND) { /* trailing edge */
        other = tg_biome_cell_index((cell + 1) * TD5_TG_BIOME_RUN);
        dist  = TD5_TG_BIOME_RUN - 1 - off;
    } else {
        return here;                              /* solid core of the run */
    }
    if (other == here) return here;

    /* dist 0 -> even odds; dist == BLEND -> always `here`. */
    thresh = (unsigned)(50 + (50 * dist) / TD5_TG_BIOME_BLEND);
    h = tg_biome_hash((unsigned)si, si, 3u);
    return ((h % 100u) < thresh) ? here : other;
}

/* Per-biome feature weighting, as a PERCENT of the emitter's own global rate:
 * 100 = unchanged, 0 = never in this biome, 200 = twice as likely.
 *
 * Offered as accessors rather than as raw table reads so the bridge and tunnel
 * emitters (owned elsewhere) need one line each and stay unaware of the biome
 * struct. Keyed on the HARD cell index, not the blended one: a bridge is a
 * hundred spans of structure and must not be decided by a dithered edge span.
 *
 * Suggested use at the emitter's existing gate:
 *     if (roll % 100 >= (base_pct * tg_biome_bridge_pct(si)) / 100) return 0; */
static int tg_biome_bridge_pct(int si)
{
    return k_biomes[tg_biome_cell_index(si)].w_bridge;
}
static int tg_biome_tunnel_pct(int si)
{
    return k_biomes[tg_biome_cell_index(si)].w_tunnel;
}
/* [R3 BLOCK] item 4: HARD cell index (not blended) -- a block turn is a shape
 * decision, so it must key off the same solid cell the road surface does. */
static int tg_biome_span_is_city(int si)
{
    return !strcmp(k_biomes[tg_biome_cell_index(si)].name, "CITY");
}

/* [R7 CITY item 7] Continuity of the city's STRUCTURAL edge -- raised sidewalk +
 * kerb HEIGHT + roadside railing, and the street-wall/tree choice that carries
 * them -- to the TRUE end of a city run. The scenery emitters normally read the
 * BLENDED biome (tg_biome_for_span), whose ~20-span dither band around a cell
 * boundary lets a trailing city span resolve to the neighbour (and a leading
 * neighbour span to the city). At a city edge that dithers the kerb height, the
 * railing and the wall/tree choice per span, and bleeds the pavement raggedly
 * into the next biome -- the railing present on one side only, the "transition
 * on span 455 doesn't keep continuity" report (seed 99991: CITY ends at 449, the
 * pavement + a one-sided railing bleed on to ~479). A raised kerb and a guard
 * rail are per-RUN structure, not a dithered categorical field, so at a CITY
 * boundary they key off the HARD cell the road surface already uses
 * (tg_biome_cell_index): solid to the last city span, then a clean edge.
 *
 * SCOPED TO CITY EDGES ONLY. Where the hard and blended biome already agree, or
 * where NEITHER is CITY, this returns the blended index unchanged -- every
 * non-city transition keeps its dither (the trees-thinning-into-buildings blend
 * this replaces only at the city line). Default ON; TD5RE_R7_CITY_CONT=0 restores
 * the fully-blended edge for an A/B. */
static int tg_biome_index_is_city(int idx)
{
    return !strcmp(k_biomes[idx].name, "CITY");
}
static int tg_scenery_biome_index(int si)
{
    int hard = tg_biome_cell_index(si);
    int soft = tg_biome_for_span(si);
    if (hard == soft) return soft;
    if (td5_env_flag_on("TD5RE_R7_CITY_CONT") &&
        (tg_biome_index_is_city(hard) || tg_biome_index_is_city(soft)))
        return hard;
    return soft;
}

/* [R5 CROSS item 12] Is span si's road surface plain tarmac? The pedestrian
 * ZEBRA page (white bars on a dark base) is authored for asphalt; on the pale
 * gravel of INDUSTRIAL or the cobble of ORIENTAL the white bars sit on an
 * already-light surface and read as a mismatched patch ("the road crossing
 * texture looks out of place ... near this road texture", span 934 = INDUSTRIAL
 * gravel). The side-street MOUTH (crossstreet asphalt) uses the biome's own road
 * page and is unaffected; only the zebra decal keys off this. */
static int tg_span_surface_is_tarmac(int si)
{
    return k_biomes[tg_biome_cell_index(si)].road_surf == RS_TARMAC;
}

/* TUNNEL EXCEPTION -- the one reason a span's surface is not its biome's.
 * Defined further down; forward-declared here because the two accessors below
 * sit above the tunnel block. */
static int tg_span_in_tunnel(int si);

/* True when span si's surface must ignore its biome and be plain tarmac.
 *
 * Weather does not fall inside a bore. In ALPINE the biome surface is RS_ICE,
 * so a tunnelled ALPINE stretch got the icy grip class AND the snow-and-ice
 * road page under a roof that no snow could ever reach -- the road inside read
 * as an ice rink. The same argument covers the other weather-ish surfaces
 * (nothing washes gravel or grows moss on a sheltered carriageway), so the rule
 * is simply: inside a tunnel, dry asphalt.
 *
 * Single definition, used by both accessors below so the GRIP the car feels and
 * the TEXTURE it drives over can never disagree. Gated so the old behaviour is
 * one env var away; default ON, it is a fix. */
static int tg_span_tunnel_tarmac(int si)
{
    return tg_span_in_tunnel(si) &&
           td5_env_flag_on("TD5RE_AUTOTRACK_TUNNEL_TARMAC");
}

/* Strip surface byte for span si: low nibble = the biome's drivable GRIP class
 * (what td5_track.c surface_type_for_span_lane reads for the centre lanes when
 * lane_bitmask is 0), high nibble left at 1 (the verge class, unused at mask 0). */
static int tg_surface_attr(int si)
{
    const TG_Biome *b;
    if (tg_span_tunnel_tarmac(si))
        return 0x10 | (k_road_surf[RS_TARMAC].grip_class & 0x0F);
    /* [R2 item 23] HARD cell index, never the blended one: the blend dithers
     * span by span, and a grip class that alternates ice/tarmac down a straight
     * is a spin, not a transition. Scenery blends; the road does not. */
    b = &k_biomes[tg_biome_cell_index(si)];
    return 0x10 | (k_road_surf[b->road_surf].grip_class & 0x0F);
}

/* Road texture page for span si, from the biome's surface. */
static int tg_road_page(int si)
{
    const TG_Biome *b;
    /* [R3 item 11] A bridge DECK is not tarmac. The biome road pages carry lane
     * paint and asphalt grain that tile down the span, and on a raised deck that
     * repeat reads as a stack of identical road tiles rather than a structure.
     * Swap in the dedicated deck page (cast concrete with a transverse seam per
     * span) so the crossing looks built, not paved. Bridge runs are near-straight
     * (item 15), so the transverse seam does not shear. NOT inside a tunnel: a
     * 40-span bridge run can overlap a 20-span tunnel run (seed 99991 has this at
     * spans 1340-1359), and there the bridge geometry is suppressed for the
     * enclosed section, so a deck seam would belong to nothing -- keep the
     * tunnel's dry tarmac. */
    if (tg_span_in_bridge_run(si) && !tg_span_in_tunnel(si))
        return TD5_TG_PAGE_BRIDGE_DECK;
    if (tg_span_tunnel_tarmac(si))
        return tg_road_slot(k_road_surf[RS_TARMAC].page_var);
    /* Same hard index as tg_surface_attr -- the TEXTURE the car drives over and
     * the GRIP it feels must never disagree, which is only guaranteed while both
     * read the same function. */
    b = &k_biomes[tg_biome_cell_index(si)];
    return tg_road_slot(k_road_surf[b->road_surf].page_var);
}

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

/* Usable pavement width for a biome, 0 = "no raised pavement here" (the signal
 * the hook reads to lay a flat verge band instead -- see tg_verge_band_w). */
static double tg_city_sidewalk_w(const TG_Biome *b)
{
    if (b->billboard || b->cell_w <= 0) return 0.0;
    return (double)b->sidewalk < TD5_TG_SIDEWALK_MIN ? TD5_TG_SIDEWALK_MIN
                                                     : (double)b->sidewalk;
}

/* Width of the FLAT verge band outside the city: "elevated sidewalks are fine,
 * but when outside the city the sidewalks can be just another texture". Tree
 * biomes get a painted-looking margin drawn on the ground rather than a slab
 * with a kerb face -- no geometry to climb, nothing to catch a wheel on, and one
 * quad per side instead of two. Biomes that HAVE a pavement get none. */
#define TD5_TG_VERGE_W      700.0   /* band width, raw                        */
#define TD5_TG_VERGE_LIFT    16.0   /* clear of the ground skirt, see below   */

static double tg_verge_band_w(const TG_Biome *b)
{
    if (tg_city_sidewalk_w(b) > 0.0) return 0.0;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_VERGE_BAND")) return 0.0;
    return TD5_TG_VERGE_W;
}

/* Rise of the pavement a facade stands on -- KERB_H only when the slab that
 * carries it is actually emitted. The wall used to add the kerb unconditionally
 * while the slab was behind TD5RE_AUTOTRACK_SIDEWALKS, so turning that knob off
 * left every building floating 130 raw over bare ground. */
static double tg_city_kerb_h(const TG_Biome *b)
{
    if (!(tg_city_sidewalk_w(b) > 0.0)) return 0.0;
    return td5_env_flag_on("TD5RE_AUTOTRACK_SIDEWALKS") ? TD5_TG_KERB_H : 0.0;
}

/* ---- FACADE CELL SIZING (the page must map at its own aspect) -------------
 * A page cell is authored at cell_w x cell_h raw (CITY: 2150 x 2950, the
 * shipped level014 measurement). The geometry can only ever fit a WHOLE number
 * of cells along a frontage, and a frontage is one span -- 1500 raw -- so at
 * CITY sizes exactly one cell covers 1500 across while the code still gave it
 * 2950 up: the page came out squashed 1500/2150 = 0.70 across, which is the
 * "building textures should not be stretched" report. Windows read tall and
 * narrow, and the error is worse the wider the authored cell is.
 *
 * The along-road extent is not ours to choose (the wall must abut the span
 * endpoints or the street wall breaks), so the FLOOR HEIGHT is what gets
 * corrected: floor_h = cell_h * (effective cell width / authored cell width).
 * The page then maps at its authored aspect and the building is simply a little
 * shorter per floor.
 *
 * The scale is computed from the NOMINAL span length, not the span's own
 * length: neighbouring spans differ by a few percent on a curve (the outer
 * setback is a longer arc), and keying height to that would saw-tooth the
 * roofline of one continuous building. Residual stretch is that same few
 * percent, which is not visible. */
static int tg_facade_cols_for(double len, double cell_w, int cap)
{
    int c;
    if (!(cell_w > 1.0)) return 1;
    c = (int)(len / cell_w + 0.5);
    if (c < 1) c = 1;
    if (c > cap) c = cap;
    return c;
}

/* Effective (as-built) cell width for biome b on a nominal-length frontage. */
static double tg_facade_cell_w(const TG_Biome *b)
{
    const double len = (double)TD5_TG_SPAN_LENGTH;
    return len / (double)tg_facade_cols_for(len, (double)b->cell_w, 4);
}

/* Height of ONE floor: the authored cell height, corrected so the page keeps
 * its aspect. Default ON; TD5RE_AUTOTRACK_FACADE_ASPECT=0 restores the raw
 * table height (and the stretch) for an A/B. */
static double tg_facade_floor_h(const TG_Biome *b)
{
    if (b->cell_w <= 0 || b->cell_h <= 0) return (double)b->cell_h;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_FACADE_ASPECT"))
        return (double)b->cell_h;
    return (double)b->cell_h * tg_facade_cell_w(b) / (double)b->cell_w;
}

/* Whole cells across a corner return. Capped at 2 on purpose: the return is
 * emitted three faces deep at every run end, so each extra column costs
 * 2*rows+1 quads on a budget (TD5_TG_FACADE_MAXQUAD) that a tall tower already
 * nearly fills. */
static int tg_facade_cap_cols(const TG_Biome *b)
{
    return tg_facade_cols_for((double)b->depth, tg_facade_cell_w(b), 2);
}

/* Building depth, quantised to whole cells for the same reason as the height:
 * a return that is not a multiple of the cell width stretches the page across
 * the flank. CITY's authored 6000 raw becomes 2 x 1500 = 3000. */
static double tg_facade_depth(const TG_Biome *b)
{
    if (b->cell_w <= 0) return (double)b->depth;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_FACADE_ASPECT"))
        return (double)b->depth;
    return (double)tg_facade_cap_cols(b) * tg_facade_cell_w(b);
}

/* Along-road depth of a run-end corner return, keyed to the facade cell so it
 * scales with the biome's building size. 0.45 of a cell is enough mass to read
 * as a corner block without closing off the side street behind it. */
static double tg_facade_cap_thick(const TG_Biome *b)
{
    /* Default ON; TD5RE_AUTOTRACK_FACADE_MASS=0 restores the old flat plane. */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_FACADE_MASS")) return 0.0;
    return (double)b->cell_w * 0.45;
}

/* Floor count of the run at span si on this side, 0 if the side is a gap. The
 * whole-block-then-tower-then-district climb was inlined in tg_side_geom; it is
 * pulled out here because the STEP-WALL pass (item 1) needs a NEIGHBOUR's height
 * without paying to build its full TG_SideGeom. tg_side_geom now calls this too,
 * so the two can never disagree on how tall a run is. */
static int tg_facade_floors(int si, int left, const TG_Biome *b)
{
    unsigned int gh, blk, ph, gs, gl;
    int floors, av;

    if (!tg_facade_built(si, left)) return 0;
    gh = tg_facade_run_id(si, left);
    floors = b->floors_min + (int)((gh >> 7) % (unsigned)b->floors_extra);
    if (b->tower_mask && ((gh >> 3) & (unsigned)b->tower_mask) == 0)
        floors += 1 + (int)((gh >> 11) % 4);          /* whole-run tower cluster */
    tg_facade_block(si, left, &blk, &ph, &gs, &gl, &av);
    floors += tg_city_district_floors(blk);
    if (floors > TD5_TG_FACADE_MAX_ROWS) floors = TD5_TG_FACADE_MAX_ROWS;
    return floors;
}

/* Streetlamp glows used to come from the prop layer as a lone additive
 * billboard at k_prop_pages[PP_LAMP].y_off = 2500 with NO POST under it, so the
 * light hung in mid-air -- the first item of the feedback. Real lamps (post +
 * arm/head + glow at the head) are emitted from tg_emit_fb_city instead, so the
 * prop layer must not also emit a bare glow. Kept as a switch rather than
 * deleting the prop block, so the old behaviour is one env var away for an A/B. */
static int tg_lamp_glow_from_props(const TG_Biome *b)
{
    if (!td5_trackgen_is_night()) return 0;    /* item 11: night tracks only */
    return b->prop_lamp && !td5_env_flag_on("TD5RE_AUTOTRACK_LAMP_POSTS");
}

/* Geometry for one side (0=right,1=left) of the wall at span si. built=0 when
 * the run/gap pattern or the branch-corridor exclusion skips this side. */
static void tg_side_geom(const TG_NodeList *nl, int si, int left,
                         const TG_Biome *b, TG_SideGeom *g)
{
    const TG_Node *n0 = &nl->v[si];
    const TG_Node *n1 = &nl->v[si + 1];
    const double side = left ? 1.0 : -1.0;
    double set0, set1, flen;
    int floors;

    g->built = 0;
    if (!tg_facade_built(si, left)) return;
    if (tg_branches_enabled() && side < 0.0 && tg_span_in_fork_clear(si)) return;
    /* [R6 item 16] Never a lone one-span building (both neighbours unbuilt). */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_NO_STUB") && tg_facade_isolated(si, left))
        return;

    /* Height is a whole number of FLOORS, keyed to the RUN so a building is one
     * uniform block that steps at the next side street. tg_facade_floors folds
     * in the tower cluster and the downtown-district climb, and clamps to
     * TD5_TG_FACADE_MAX_ROWS -- a real ceiling, not cosmetic: at cols == 1 (every
     * city biome, a 1500-raw span against a 2150-raw cell) a run costs rows quads
     * for the front, rows for a tower back wall and 2*(2*rows + rows + 2) for the
     * two corner prisms -- about 12*rows + 4 per side. 10 floors keeps both sides
     * inside TD5_TG_FACADE_MAXQUAD; past it tg_facade_push_grid silently drops
     * quads, which shows up as buildings missing their top floors. */
    floors   = tg_facade_floors(si, left, b);
    g->rows  = floors;
    g->H     = (double)floors * tg_facade_floor_h(b);
    g->depth = tg_facade_depth(b);

    g->lx0 = n0->tz * side; g->lz0 = -n0->tx * side;
    g->lx1 = n1->tz * side; g->lz1 = -n1->tx * side;
    /* Setback is the PAVEMENT width (tg_city_sidewalk_w), not the raw table
     * field: the wall must land on the back edge of the slab the hook lays, or
     * one of the two is left hanging. Base rises by the kerb for the same
     * reason -- the wall stands ON the pavement, not in the gutter.
     *
     * The setback is pushed out through the SHARED carriageway authority rather
     * than derived from a fixed width: BRANCH makes the corridor width per-fork
     * variable, so a facade keying off a constant road half-width would drift
     * onto a widened branch (the round-3 item 9 "buildings spawned ON the
     * branches"). tg_carriageway_clear_gap is the documented adoption for
     * facades and re-derives no fork arithmetic; it returns the pavement width
     * unchanged wherever the road edge is already the outermost tarmac -- every
     * span off a fork, and the whole +ve lateral -- so straight city blocks are
     * byte-identical. The right-side facade in a fork's cleared region is still
     * dropped by the tg_span_in_fork_clear guard above; this only hardens the
     * setback everywhere else, and keeps the whole building (front, caps and the
     * step wall, which all derive from set0/set1) off any carriageway. */
    {
        const double gap = tg_carriageway_clear_gap(nl, si, side,
                               tg_city_sidewalk_w(b), TD5_TG_CARRIAGEWAY_MARGIN);
        set0 = n0->width * 0.5 + gap;
        set1 = n1->width * 0.5 + gap;
    }

    g->bx = n0->x + g->lx0 * set0;
    g->by = n0->y + tg_city_kerb_h(b);
    g->bz = n0->z + g->lz0 * set0;
    g->ax = (n1->x + g->lx1 * set1) - g->bx;
    g->ay = n1->y - n0->y;
    g->az = (n1->z + g->lz1 * set1) - g->bz;

    flen = sqrt(g->ax * g->ax + g->az * g->az);
    if (flen < 1.0) flen = 1.0;
    g->cols = tg_facade_cols_for(flen, (double)b->cell_w, 4);

    /* A cap closes a flank whose neighbour side is NOT actually built -- which
     * includes a fork-cleared span (item 15), not just a run/gap boundary.
     * tg_side_built folds that in; on the left side and off any fork it is
     * exactly tg_facade_built, so straight blocks are unchanged. */
    g->cap_near = !tg_side_built(si - 1, left);
    g->cap_far  = !tg_side_built(si + 1, left);
    g->built = 1;
}

static int tg_emit_street_wall(const TG_NodeList *nl, int si,
                               const TG_Biome *b, TG_Buf *blk)
{
    double px[TD5_TG_FACADE_MAXQUAD * 4], py[TD5_TG_FACADE_MAXQUAD * 4];
    double pz[TD5_TG_FACADE_MAXQUAD * 4], uu[TD5_TG_FACADE_MAXQUAD * 4];
    double vv[TD5_TG_FACADE_MAXQUAD * 4];
    TG_SideGeom sd[2];
    /* Pages are keyed to the RIGHT side's run: both sides share one mesh, so
     * they cannot carry different pages, and the run id at least keeps a whole
     * building on one texture across the side streets. */
    unsigned int block_gh = tg_facade_run_id(si, 0);
    const double cap_thick = tg_facade_cap_thick(b);
    const int cap_cols = tg_facade_cap_cols(b);
    int n = 0, n_store, s, nseg, seg_page[2], seg_nq[2], wall_rows, step_max = 0;

    if (si + 1 >= nl->count) return 1;    /* need the far endpoint to abut */
    tg_side_geom(nl, si, 0, b, &sd[0]);
    tg_side_geom(nl, si, 1, b, &sd[1]);

    /* STOREFRONT pass: the ground floor (row 0) of each front plane goes FIRST
     * in the vertex list, so a leading command can bind the shop page. */
    for (s = 0; s < 2; s++) {
        TG_SideGeom *g = &sd[s];
        if (!g->built) continue;
        tg_facade_push_grid(g->bx, g->by, g->bz, g->ax, g->ay, g->az,
                            0.0, g->H, 0.0, g->cols, g->rows, 0, 1,
                            px, py, pz, uu, vv, &n);
    }
    n_store = n;

    /* FACADE pass: the floors ABOVE the shop, plus the deep return walls that
     * turn the corner at run ends (the multi-sided buildings). */
    for (s = 0; s < 2; s++) {
        TG_SideGeom *g = &sd[s];
        if (!g->built) continue;
        tg_facade_push_grid(g->bx, g->by, g->bz, g->ax, g->ay, g->az,
                            0.0, g->H, 0.0, g->cols, g->rows, 1, g->rows,
                            px, py, pz, uu, vv, &n);
        /* MASS, on every span of the run rather than only at its ends. The
         * front plane is a zero-thickness sheet: at run INTERIOR spans there was
         * nothing at all behind it, so a tall run seen from a rise, a crest or
         * an approaching curve showed one face and a knife-edge roofline -- the
         * "taller buildings only have a facade ... just one visible face"
         * report. Two faces close it for the cost of a roof quad plus, on
         * TOWERS only, a back wall:
         *   ROOF  -- always. Four explicit corners (the back line follows the
         *            frontage through the curve), so the deck cannot gap at the
         *            joint the way a single-lateral parallelogram would.
         *   BACK  -- towers only. A low block is hidden by the back rows behind
         *            it, but a tower stands over them and was see-through from
         *            behind and down every cross street. Gated by height so the
         *            quad budget is only spent where it shows. */
        if (g->depth > 1.0 && td5_env_flag_on("TD5RE_AUTOTRACK_FACADE_MASS")) {
            const double d = g->depth;
            double q[12];
            q[0] = g->bx;                  q[1]  = g->by + g->H;
            q[2] = g->bz;
            q[3] = g->bx + g->ax;          q[4]  = g->by + g->ay + g->H;
            q[5] = g->bz + g->az;
            q[6] = g->bx + g->ax + g->lx1 * d; q[7] = g->by + g->ay + g->H;
            q[8] = g->bz + g->az + g->lz1 * d;
            q[9] = g->bx + g->lx0 * d;     q[10] = g->by + g->H;
            q[11] = g->bz + g->lz0 * d;
            tg_facade_push_quad(q, px, py, pz, uu, vv, &n);

            if (g->rows >= TD5_TG_FACADE_TALL_ROWS) {
                const double bx2 = g->bx + g->lx0 * d, bz2 = g->bz + g->lz0 * d;
                tg_facade_push_grid(bx2, g->by, bz2,
                                    (g->bx + g->ax + g->lx1 * d) - bx2, g->ay,
                                    (g->bz + g->az + g->lz1 * d) - bz2,
                                    0.0, g->H, 0.0, g->cols, g->rows, 0, g->rows,
                                    px, py, pz, uu, vv, &n);
            }
        }
        if (g->cap_near || g->cap_far) {
            /* Along-road unit, needed to give the corner prism its thickness.
             * Clamped to most of the span so a return can never overrun the
             * span it belongs to and poke out of the far end of the run. */
            const double alen = sqrt(g->ax * g->ax + g->az * g->az);
            const double aux = (alen > 1.0) ? g->ax / alen : 0.0;
            const double auz = (alen > 1.0) ? g->az / alen : 1.0;
            double thick = cap_thick;
            if (alen > 1.0 && thick > alen * 0.9) thick = alen * 0.9;
            if (g->cap_near)
                tg_facade_push_cap(g->bx, g->by, g->bz, g->lx0, g->lz0,
                                   aux, auz, g->depth, thick, g->H, g->rows,
                                   cap_cols, px, py, pz, uu, vv, &n);
            if (g->cap_far)
                tg_facade_push_cap(g->bx + g->ax, g->by + g->ay, g->bz + g->az,
                                   g->lx1, g->lz1, -aux, -auz,
                                   g->depth, thick, g->H, g->rows,
                                   cap_cols, px, py, pz, uu, vv, &n);
        }
        /* SIDE WALL at a height STEP (item 1). Two ADJACENT built runs of
         * different height share a flush frontage, but the taller one's flank
         * ABOVE the shorter roof was open air, so a driver looked straight into
         * the building -- the "buildings with different height look like they are
         * hollow inside" report. A run END (the neighbour is a gap) is already
         * closed by the corner prism above; this closes a run-to-run height
         * CHANGE, which the prisms never touched because both spans are BUILT.
         *
         * Emitted only at the FAR boundary (si|si+1) so each seam is filled
         * exactly once -- the near boundary is the previous span's far boundary --
         * and only across the EXPOSED band [shorter roof, taller roof], so no quad
         * is spent where the two roofs already meet flush. The face lies in the
         * lateral-vertical plane at the shared endpoint, its depth the building's
         * own, so from either approach direction it caps the step. Same page as
         * the facade above (it IS the building's side); grouped after the
         * storefront so it lands in the wall command.
         *
         * Dedicated knob TD5RE_AUTOTRACK_STEP_WALLS (default on) so this one
         * feature can be A/B'd in isolation from the rest of the FACADE_MASS
         * pass. CARRIAGEWAY CLEARANCE is inherited: the base derives from set1,
         * which tg_side_geom now pushes out through the shared authority, so the
         * seam can no more land on a branch than the front facade can -- no
         * separate guard is needed here. */
        if (g->built && td5_env_flag_on("TD5RE_AUTOTRACK_FACADE_MASS") &&
            td5_env_flag_on("TD5RE_AUTOTRACK_STEP_WALLS") &&
            tg_side_built(si + 1, s)) {
            const int nrows = tg_facade_floors(si + 1, s, b);
            if (nrows > 0 && nrows != g->rows) {
                const int hi = nrows > g->rows ? nrows : g->rows;
                const int lo = nrows < g->rows ? nrows : g->rows;
                const double fh = tg_facade_floor_h(b);
                const double fx = g->bx + g->ax, fy = g->by + g->ay,
                             fz = g->bz + g->az;
                tg_facade_push_grid(fx, fy, fz,
                                    g->lx1 * g->depth, 0.0, g->lz1 * g->depth,
                                    0.0, (double)hi * fh, 0.0,
                                    cap_cols, hi, lo, hi,
                                    px, py, pz, uu, vv, &n);
                if (nrows > step_max) step_max = nrows;
                tg_acct(TG_ACCT_STEPWALL, si);
            }
        }
    }

    if (n <= 0) return 1;
    /* Pages keyed to the PERIOD-block (a run lives inside one block) so a whole
     * building keeps one shop + one wall page and neighbours differ. Ground
     * quads [0,n_store) sample the shop page, the rest the wall page. */
    /* Page CLASS follows the taller of the two sides -- they share one mesh, so
     * they share one wall page, and a tower opposite a shop row should read as
     * the tower it is. */
    wall_rows = sd[0].built ? sd[0].rows : 0;
    if (sd[1].built && sd[1].rows > wall_rows) wall_rows = sd[1].rows;
    /* A step wall borrowed from a taller NEIGHBOUR run can be the tallest thing
     * in the mesh; let it pull the wall page up to the tower class so the flank
     * matches the office block it belongs to rather than the low run beside it. */
    if (step_max > wall_rows) wall_rows = step_max;
    if (n_store > 0 && n_store < n) {
        seg_page[0] = tg_store_page(block_gh);  seg_nq[0] = n_store / 4;
        seg_page[1] = tg_facade_page_class(block_gh, wall_rows);
        seg_nq[1] = (n - n_store) / 4;
        nseg = 2;
    } else {
        seg_page[0] = (n_store >= n) ? tg_store_page(block_gh)
                                     : tg_facade_page_class(block_gh, wall_rows);
        seg_nq[0] = n / 4;
        nseg = 1;
    }
    /* One mesh, but up to one frontage per side, and a ground-floor storefront
     * command only when the run actually got one. */
    tg_acct_n(TG_ACCT_BUILDING, si,
              (sd[0].built ? 1 : 0) + (sd[1].built ? 1 : 0));
    if (n_store > 0) tg_acct(TG_ACCT_SHOPFRONT, si);
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, seg_page, seg_nq, nseg);
}

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
static int tg_facade_stands(int si)
{
    if (si <= 0) return 0;
    if (tg_span_in_bridge_run(si)) return 0;
    return tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(si)]) > 0.0;
}

/* [R7 item 18] Ground/water validity + base-Y for a verge tree billboard.
 * Defined after the terrain cross-section (tg_ground_side); forward-declared here
 * so both tree emitters can share it. Returns 0 when the trunk is over water or a
 * coastline (skip the tree), else writes the plant point and sampled ground Y. */
static int tg_flora_plant(const TG_NodeList *nl, int si, const TG_Biome *b,
                          double side, double gap, double tw,
                          double *cx, double *cz, double *base_y);

static int tg_building_for_span(const TG_NodeList *nl, int si, TG_Buf *blk)
{
    unsigned int h = (unsigned)si * 2654435761u;
    const TG_Biome *b;
    const TG_TreePage *tp;
    double side, gap, tw, th, jit, cx, cz;
    int tv;

    /* Only span 0 is kept clear, not the whole grid stretch. The old
     * `si <= TD5_TG_GRID_SPAN` skipped 24 spans -- 36000 raw, the entire
     * opening straight -- so the race began in bare ground and the buildings
     * only cut in once the player was already moving. Nothing here is ever ON
     * the road (the wall sits behind the pavement), so the grid does not need
     * the clearance; it only needs somewhere for the near cap to end. */
    if (si <= 0) return 1;
    if (tg_span_in_bridge_run(si)) return 1;   /* deck is clear -- see the river */
    /* [R7 item 7] wall-vs-tree keyed on the hardened city edge so the frontage
     * ends cleanly with its sidewalk, not a span early/late (see the helper). */
    b = &k_biomes[tg_scenery_biome_index(si)];

    if (!b->billboard || b->tree_n <= 0)
        return tg_emit_street_wall(nl, si, b, blk);

    /* Trees: density-gated camera-facing billboards. Each biome MIXES several
     * species (tree_set) picked per-tree, each with its own shipped size, and a
     * scale jitter that keeps the page aspect. */
    if ((int)(h >> 28) > b->density) return 1;

    side = ((h >> 3) & 1) ? 1.0 : -1.0;
    tv  = b->tree_set[(h >> 13) % (unsigned)b->tree_n];
    tp  = &k_tree_pages[tv];

    jit = 0.8 + (double)((h >> 9) % 41) * 0.01;    /* 0.80 .. 1.20 */
    tw  = (double)tp->w * jit;
    th  = (double)tp->h * jit;
    gap = 800.0 + (double)((h >> 5) % 2400);       /* set back off the verge */
    gap = tg_flora_gap_clear(nl, si, side, gap);    /* never on a branch */

    {   /* [R7 item 18] on the ground, never on water/coastline */
        double base_y;
        if (!tg_flora_plant(nl, si, b, side, gap, tw, &cx, &cz, &base_y))
            return 1;

        tg_flora_diag(nl, si, "near", tg_tree_slot(tv), side, tw, th, cx, cz);
        tg_acct(TG_ACCT_TREE, si);
        return tg_emit_billboard_mesh(blk, cx, base_y, cz, tw * 0.5, th,
                                      tg_tree_slot(tv), 1);
    }
}

/* A verge side that the branch corridor bows into over the fork span range --
 * suppress props there for the same reason as facades/trees. */
static int tg_side_blocked(int si, double side)
{
    return tg_branches_enabled() && side < 0.0 && tg_span_in_fork_clear(si);
}

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
 * TD5RE_R7_PRE_BRANCH_PAVE=0 restores the blanket fork-clear drop for an A/B. */
static int tg_pavement_side_blocked(const TG_NodeList *nl, int si, double side)
{
    if (!tg_branches_enabled() || side >= 0.0) return 0;
    if (!tg_span_in_fork_clear(si)) return 0;
    if (!td5_env_flag_on("TD5RE_R7_PRE_BRANCH_PAVE")) return 1;
    return tg_carriageway_reach(nl, si, side)
         > tg_road_half_width(nl, si) + 1.0;
}

/* Emit one prop billboard (prop-page index pp) beside span si on `side`, `gap`
 * world units past the road edge, recording its mesh offset. */
static int tg_prop_one(const TG_NodeList *nl, int si, int pp, double side,
                       double gap, TG_Buf *m, size_t *moff, int *pn)
{
    const TG_Node *n = &nl->v[si];
    const TG_PropPage *P = &k_prop_pages[pp];
    double lx = n->tz * side, lz = -n->tx * side;
    double cx = n->x + lx * (n->width * 0.5 + gap);
    double cz = n->z + lz * (n->width * 0.5 + gap);
    size_t b0 = m->len;

    if (tg_side_blocked(si, side)) return 1;
    moff[*pn] = b0;
    if (!tg_emit_billboard_mesh(m, cx, n->y + (double)P->y_off, cz,
                                (double)P->w * 0.5, (double)P->h,
                                tg_prop_slot(pp), P->tag))
        return 0;
    /* PP_LAMP is the streetlamp GLOW, emitted through this same helper by the
     * lamp fixture (and by the props layer when the biome puts glows there).
     * It is accounted as a LAMP at the fixture, so keep it out of the prop
     * bucket or every lamp shows up twice under two different kinds. */
    if (m->len > b0) {
        (*pn)++;
        if (pp != PP_LAMP) tg_acct(TG_ACCT_PROP, si);
    }
    return 1;
}

/* Spectator density for biome `b`, overriding the shared table's prop_people.
 * The gate below fires when (hash>>28) <= density, so density d covers (d+1)/16
 * of spans. The table put crowds in four biomes -- CITY 6 (44% of spans),
 * COAST 6 (44%), ORIENTAL 4 (31%), INDUSTRIAL 3 (25%) -- which reads as a
 * permanent crowd lining an empty industrial road or a temple lane. A crowd is
 * a city (and seafront promenade) thing, so those keep a real density and
 * everywhere else drops to a rare passer-by:
 *   CITY 6 -> 44%   COAST 3 -> 25%   any other crowded biome 1 -> 12.5%
 * An accessor rather than an edit to k_biomes, so the shared table stays a
 * single definition; matched on name so it survives a table reorder. */
static int tg_people_density(const TG_Biome *b)
{
    if (b->prop_people <= 0) return 0;             /* biome wants none */
    if (!strcmp(b->name, "CITY"))  return b->prop_people;
    if (!strcmp(b->name, "COAST")) return 3;       /* promenade, not a grandstand */
    return 1;
}

/* Roadside prop layer for span si (spectators, streetlamps, statues, animals),
 * additional to the trees/facades. Each emitted billboard records its own mesh
 * offset, so props may be a variable count of differently-sized meshes. */
static int tg_emit_props(const TG_NodeList *nl, int si, const TG_Biome *b,
                         TG_Buf *m, size_t *moff, int *pn, int cap)
{
    unsigned int h = (unsigned)si * 0x9E3779B9u;   /* independent of the tree hash */
    const int people = tg_people_density(b);
    double side;

    if (tg_span_in_bridge_run(si)) return 1;   /* nothing on the deck but rails */

    /* People: spectators on the sidewalk, sometimes a pair. */
    if (people > 0 && (int)(h >> 28) <= people && *pn < cap) {
        int pp = PP_PERSON0 + (int)((h >> 5) & 1);
        side = ((h >> 3) & 1) ? 1.0 : -1.0;
        if (!tg_prop_one(nl, si, pp, side, 400.0 + (double)((h >> 6) % 800),
                         m, moff, pn)) return 0;
        if (((h >> 20) & 1) && *pn < cap &&
            !tg_prop_one(nl, si, pp ^ 1, side,
                         1000.0 + (double)((h >> 7) % 700), m, moff, pn))
            return 0;
    }
    /* Streetlamp glows: periodic, both curbs (additive). */
    if (tg_lamp_glow_from_props(b) && (si % 7) == 0) {
        int s;
        for (s = 0; s < 2 && *pn < cap; s++)
            if (!tg_prop_one(nl, si, PP_LAMP, s ? 1.0 : -1.0, 300.0,
                             m, moff, pn)) return 0;
    }
    /* Statue / monument: sparse landmark. */
    if (b->prop_statue >= 0 && (si % 29) == 0 && *pn < cap) {
        side = ((h >> 9) & 1) ? 1.0 : -1.0;
        if (!tg_prop_one(nl, si, b->prop_statue, side, 1500.0, m, moff, pn))
            return 0;
    }
    /* Animals: low density, set well back off the verge. */
    if (b->prop_animal >= 0 && (int)(h >> 28) <= 2 && *pn < cap) {
        side = ((h >> 11) & 1) ? 1.0 : -1.0;
        if (!tg_prop_one(nl, si, b->prop_animal, side,
                         2500.0 + (double)((h >> 12) % 4000), m, moff, pn))
            return 0;
    }
    return 1;
}

/* ===================== WATER =====================
 * A flat sea/river plane on the SEAWARD side of a coastal run: it starts past a
 * beach strip (so palms on the verge stand on the sand, not in the water) and
 * runs far out, a bit below the road. Shipped coastal tracks (level012 sea,
 * level021 coast) do exactly this -- a big flat blue mesh grid below road level.
 * Raw = world*256. */
#define TD5_TG_WATER_DROP    1200    /* how far below the road the surface sits  */
#define TD5_TG_WATER_BEACH   8100    /* gap from the road edge to the shoreline  */
#define TD5_TG_WATER_EXTENT  50000   /* how far out to sea the plane reaches     */
/* World units per page repeat. The sea is textured by WORLD POSITION, not by
 * cell, so the page tiles on a single global grid (see tg_emit_water). */
#define TD5_TG_WATER_TILE    6000.0

/* Which side of a coastal run the sea is on -- fixed per biome-run so the coast
 * does not flip sides mid-stretch. */
static double tg_water_side(int si)
{
    unsigned int h = (unsigned)(si / TD5_TG_BIOME_RUN) * 2246822519u;
    return (h & 1) ? 1.0 : -1.0;
}

/* Sea level for the coastal run containing span si -- ONE height for the whole
 * run, not per span.
 *
 * Root cause of the "marching" sea: the surface used to be n->y - WATER_DROP,
 * i.e. it followed the road's own elevation profile, so the sea rose and fell
 * with every hill. A body of water is level by definition, so take the LOWEST
 * road node in the biome run and sit below that -- below the road everywhere in
 * the run, so no low point is ever flooded. */
static double tg_sea_level_y(const TG_NodeList *nl, int si)
{
    const int run = si / TD5_TG_BIOME_RUN;
    int a = run * TD5_TG_BIOME_RUN;
    int b = a + TD5_TG_BIOME_RUN - 1;
    double lo;
    int i;

    if (a > nl->count - 1) a = nl->count - 1;
    if (b > nl->count - 1) b = nl->count - 1;
    lo = nl->v[a].y;
    for (i = a + 1; i <= b; i++) if (nl->v[i].y < lo) lo = nl->v[i].y;
    return lo - (double)TD5_TG_WATER_DROP;
}

/* May a water quad be laid across span si at all?
 *
 * Reported: "i see water on top of the tunnel". Neither water emitter consulted
 * the tunnel gate. Both are called from the NON-tunnel branch of the emit loop,
 * which looks like it settles the question but does not: every water quad runs
 * from node si to node si+1, so the last open span before a portal lays its
 * plane across the FIRST span of the tunnel run. The sea gets away with it (it
 * starts TD5_TG_WATER_BEACH outboard of the road edge, so it never reaches the
 * bore) but the river spans the full TD5_TG_BRIDGE_WATER_HALF either side of the
 * centreline and goes straight through it.
 *
 * A tunnel is an enclosed section cut into rock; nothing floats over it and
 * nothing runs through it. Cheaper and more honest to refuse the quad than to
 * try to clip it. TD5RE_AUTOTRACK_WATER_TUNNEL_GATE=0 restores the old
 * behaviour for comparison. */
static int tg_water_span_clear(int si)
{
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_WATER_TUNNEL_GATE")) return 1;
    return !tg_span_in_tunnel(si) && !tg_span_in_tunnel(si + 1);
}

/* Emit the sea plane beside span si on `side`, recording its mesh offset.
 *
 * ONE quad, and its UVs come from the WORLD x/z of each corner rather than
 * running 0..1 per cell. Both changes fix the same fault: a 2 x 6 cell grid per
 * span mapped the page once per cell, so every cell boundary was a seam and the
 * whole field of them slid past as a marching pattern. Projecting the page onto
 * the world XZ grid instead makes neighbouring spans agree at their shared edge
 * by construction, so the sea reads as one continuous body. It is also 4
 * vertices instead of 48. */
static int tg_emit_water(const TG_NodeList *nl, int si, double side,
                         TG_Buf *m, size_t *moff, int *pn)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    const TG_Node *n0 = &nl->v[si];
    const TG_Node *n1;
    double lx0, lz0, lx1, lz1, e0, e1, wy;
    int i, seg_page = TD5_TG_PAGE_WATER, seg_nq = 1;

    if (si + 1 >= nl->count) return 1;
    if (tg_side_blocked(si, side)) return 1;
    if (!tg_water_span_clear(si)) return 1;
    n1 = &nl->v[si + 1];

    lx0 = n0->tz * side; lz0 = -n0->tx * side;
    lx1 = n1->tz * side; lz1 = -n1->tx * side;
    e0 = n0->width * 0.5 + (double)TD5_TG_WATER_BEACH;
    e1 = n1->width * 0.5 + (double)TD5_TG_WATER_BEACH;
    wy = tg_sea_level_y(nl, si);

    /* shore-near, shore-far, sea-far, sea-near: the same ring order the cell
     * grid produced, so the face keeps whatever winding was drawing before. */
    px[0] = n0->x + lx0 * e0;                        pz[0] = n0->z + lz0 * e0;
    px[1] = n1->x + lx1 * e1;                        pz[1] = n1->z + lz1 * e1;
    px[2] = px[1] + lx1 * (double)TD5_TG_WATER_EXTENT;
    pz[2] = pz[1] + lz1 * (double)TD5_TG_WATER_EXTENT;
    px[3] = px[0] + lx0 * (double)TD5_TG_WATER_EXTENT;
    pz[3] = pz[0] + lz0 * (double)TD5_TG_WATER_EXTENT;
    for (i = 0; i < 4; i++) {
        py[i] = wy;
        uu[i] = px[i] / TD5_TG_WATER_TILE;
        vv[i] = pz[i] / TD5_TG_WATER_TILE;
    }

    moff[*pn] = m->len;
    if (!tg_write_quad_mesh(m, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
        return 0;
    (*pn)++;
    tg_acct_range(TG_ACCT_WATER, si, si + 1);   /* sea or river plane */
    return 1;
}

/* Tunnels come in runs so a whole stretch is enclosed, not isolated spans. */
#define TD5_TG_TUNNEL_RUN  20

/* [R4 item 19] Spans of ordinary ground a tunnel run must keep clear of any
 * bridge run, on either side. A deck runs into a bore with zero clearance today
 * (seed 99991: bridge 1320-1359 overlaps tunnel 1340-1359), so this both forbids
 * the OVERLAP and guarantees a flat approach band between the two -- ~10 spans
 * (~15000 world units) is a full bridge approach length. */
#define TD5_TG_BRIDGE_TUNNEL_CLEAR 10

static int tg_span_in_tunnel(int si)
{
    unsigned int h;
    /* OFF BY DEFAULT -- emitted but NEVER VERIFIED IN FRAME. A test run with
     * tunnels on showed a dark slab near the road that turned out to be a tall
     * BUILDING (tunnels off, still present), so no frame has yet confirmed a
     * tunnel appearing at all -- neither working nor broken. Off until someone
     * drives into a known tunnel run and looks.
     *
     * Known RISK, from the format survey rather than observation: there is no
     * engine support for interior darkening or occlusion, so the roof will be
     * lit from outside; and every span in the run gets an identical section,
     * so there is no MOUTH and the near end may read as a wall.
     * Enable with TD5RE_AUTOTRACK_TUNNELS=1 to work on them. */
    /* Default ON (2026-08-26); set TD5RE_AUTOTRACK_TUNNELS=0 to disable. */
    unsigned int thresh;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_TUNNELS")) return 0;
    if (si <= TD5_TG_GRID_SPAN + 40) return 0;      /* not right off the grid */
    h = (unsigned)(si / TD5_TG_TUNNEL_RUN) * 2246822519u;
    /* Same shape as the bridge gate: 125/1000 is the old ~1-in-8, scaled by the
     * biome weight, so ALPINE at 230 bores ~29% of runs and FIELDS at 10 gets
     * ~1% ("mountains = tunnels"). */
    thresh = (125u * (unsigned)tg_biome_tunnel_pct(si)) / 100u;
    if (thresh > 1000u) thresh = 1000u;
    if (((h >> 8) % 1000u) >= thresh) return 0;     /* this run does not bore */

    /* [R4 item 19] INTERLOCK. The bridge gate (tg_span_in_bridge_run) and this
     * one are independent hash draws with no shared state, so nothing stopped a
     * bridge run and a tunnel run from overlapping -- seed 99991 has bridge run
     * 1320-1359 and tunnel run 1340-1359 both firing, and the raised deck drives
     * straight into the bore, which is illegal terrain. Resolve it one way: the
     * BRIDGE is the authored crossing and keeps its span; the tunnel YIELDS. A
     * tunnel run is suppressed if any span within CLEAR of it lies in a bridge
     * run, which forbids the overlap AND leaves a flat approach band between the
     * two. Stateless and derived only from si, so the elevation pass, the strip
     * builder and every scenery gate agree without passing anything around. A
     * bridge weight tweak could only make the collision rarer; this makes it
     * impossible. */
    {
        const int t0 = (si / TD5_TG_TUNNEL_RUN) * TD5_TG_TUNNEL_RUN;
        const int t1 = t0 + TD5_TG_TUNNEL_RUN - 1;
        int s, lo = t0 - TD5_TG_BRIDGE_TUNNEL_CLEAR;
        const int hi = t1 + TD5_TG_BRIDGE_TUNNEL_CLEAR;
        if (lo < 0) lo = 0;
        for (s = lo; s <= hi; s++)
            if (tg_span_in_bridge_run(s)) return 0;
    }
    return 1;
}

/* Lining page for the tunnel RUN containing si (item 16a).
 *
 * The report was that every bore wears the one concrete page, so a long
 * tunnelled stretch reads as a copy-paste. There is no per-bore art to draw on
 * (tunnels are port-original), so instead we generate several concrete linings
 * that differ in tone/grain (tg_emit_texture_page_tunnel_var) and hand each RUN
 * a different one. Keyed on si/RUN, not si, so a single bore is ONE lining end
 * to end -- a lining that changed mid-tunnel would be the worse artifact. The
 * base TD5_TG_PAGE_TUNNEL is variant 0 so the existing page is never wasted. */
static int tg_tunnel_lining_page(int si)
{
    /* [R6 TUNNEL item 8c] Repointed to the R6 cast-concrete lining family (4
     * variants) that replaces the checkerboard-reading grain of the old
     * TD5_TG_PAGE_TUNNEL(_VAR) pages. Still one lining per RUN (keyed on
     * si/RUN) so a bore is a single lining end to end. TD5RE_AUTOTRACK_
     * TUNNEL_OLDLINING=1 restores the old pages for an A/B. */
    unsigned int h = (unsigned)(si / TD5_TG_TUNNEL_RUN) * 2654435761u;
    if (td5_env_flag_off("TD5RE_AUTOTRACK_TUNNEL_OLDLINING")) {
        unsigned int ov = (h >> 13) % (unsigned)TD5_TG_TUNNEL_VARIANTS;
        if (ov == 0) return TD5_TG_PAGE_TUNNEL;
        return TD5_TG_PAGE_TUNNEL_VAR + (int)(ov - 1);
    }
    return TD5_TG_PAGE_R6_TUNNEL + (int)((h >> 13) % 4u);
}

/* Clear interior height of the bore, and the thickness of wall/roof slabs.
 * Named because the mountain massing above the tunnel (tg_emit_fb_tunnel) has
 * to stack on top of the roof and cannot guess these. */
#define TD5_TG_TUNNEL_HEIGHT  2600.0
#define TD5_TG_TUNNEL_WALL_T   300.0

/* Fork whose MAIN half-carriageway covers a main-ring span. Owned by the branch
 * area and defined further down, forward-declared here because the bore has to
 * know whether a branch runs alongside it. READ ONLY from the tunnel code. */
static int tg_fork_of_main(int si);

/* Lateral extent the bore has to enclose at main-ring span si: *half is the
 * clear half-width, *shift the centre offset from the road centreline (+ve =
 * LEFT of travel, matching tg_append_row's sign).
 *
 * Normally that is just the road (half = width/2, centre 0). But where a BRANCH
 * fork runs through a tunnelled stretch, the main-ring span carries only the
 * LEFT half carriageway -- shifted to +width/4 by TD5_TG_MAIN_SHIFT, so it
 * occupies [0, +width/2] -- while the branch corridor is a SECOND drivable
 * carriageway at the same span, bowing out to the right to tg_branch_shift(k)
 * (down to -(0.25 + TD5_TG_BRANCH_BOW) * width at the bow peak). A bore sized
 * for the road alone therefore puts its left wall straight through the branch,
 * which is the clipping the feedback describes.
 *
 * So enclose the UNION of the two carriageways and re-centre the bore on it:
 * one wide cavern carrying both, rather than a box one of them passes through.
 *
 * DERIVED CONSEQUENCE, worth knowing before looking at it: with BRANCH_BOW 1.20
 * the union at the bow peak runs from -1.70*width to +0.50*width, i.e. 2.2x the
 * road's own width and offset 0.6*width to the right. A fork inside a tunnel is
 * a genuinely wide cavern, not a road tunnel. Capped below so a long fork
 * cannot demand an unbuildable span. */
static void tg_tunnel_bore(const TG_NodeList *nl, int si,
                           double *half, double *shift)
{
    const double w = nl->v[si].width;
    int fi;

    *half  = w * 0.5;
    *shift = 0.0;
    if (!tg_branches_enabled()) return;

    fi = tg_fork_of_main(si);
    if (fi < 0) return;                     /* no branch alongside this span */
    {
        /* Corridor step paired with this main span, same indexing tg_emit_models
         * uses for the corridor road (j = si - F - 1). */
        const int L = s_forks[fi].len;
        const int j = si - s_forks[fi].F - 1;
        const double br = tg_branch_shift(j, L, w);      /* branch centre, -ve */
        const double lo = br - w * 0.25;                 /* right edge of union */
        const double hi = w * 0.5;                       /* left edge of union */
        double h = (hi - lo) * 0.5;
        /* Cap: the roof is one flat slab, and past ~3x the road width it reads
         * as a ceiling over open ground rather than a bore. Beyond the cap the
         * branch simply runs outside the tunnel, which is at least not a wall
         * through the road. */
        const double cap = w * 1.5;
        if (h > cap) h = cap;
        *shift = (lo + hi) * 0.5;
        *half  = h;
    }
}

/* [R5 STRUCT item 9] Sweep the tunnel walls/roof between consecutive nodes.
 *
 * The box version below placed one axis-aligned box at EACH node, oriented by
 * that node's own straight tangent only. On a curving or graded bore those
 * rigid boxes diverge from the centreline: on the inside of a curve the wall
 * pulls away and daylight shows through, on the outside it juts across the road,
 * and the mismatched slabs read as tilted "slopes" -- verbatim item 9, and
 * visible in log/r5s_tunnel_b.png (road curving left, left wall gone, blocks
 * scattered). Sweeping the two side walls and the roof from node si to node si+1
 * -- the same way the road quads are built -- makes the enclosure follow the
 * road in both plan and grade. Walls are single inner-face planes (scenery is
 * CULL_NONE so they draw solid from inside), the roof caps between the wall
 * tops, and a bright lintel closes each run mouth. Default ON;
 * TD5RE_R5_TUNNEL_SWEEP=0 restores the boxes for an A/B. */
static int tg_r5_tunnel_sweep(void)
{
    return td5_env_flag_on("TD5RE_R5_TUNNEL_SWEEP");   /* default ON */
}

static int tg_emit_tunnel_swept(const TG_NodeList *nl, int si, TG_Buf *blk,
                                int *added)
{
    const double wall_t = TD5_TG_TUNNEL_WALL_T;
    const double height = TD5_TG_TUNNEL_HEIGHT;
    const unsigned int dim = 0xFF585868u;   /* shadowed interior */
    const unsigned int lampc = 0xFFFFE8B4u; /* warm-white lamp glow (ARGB) */
    const int lining  = tg_tunnel_lining_page(si);
    const int lamp_pg = TD5_TG_PAGE_R6_TUNNEL + 5;      /* [item 8d] */
    /* [R7 item 9] portal facade page (was the R6 thin-lintel page +4). */
    const int portal_pg = td5_env_flag_on("TD5RE_AUTOTRACK_R7_PORTAL")
                        ? TD5_TG_PAGE_R7_BRIDGE + 0 : TD5_TG_PAGE_R6_TUNNEL + 4;
    const int lamps_on = td5_env_flag_on("TD5RE_AUTOTRACK_TUNNEL_LAMPS");
    const double tile = 3000.0;
    const TG_Node *nd = &nl->v[si];
    double px[64], py[64], pz[64], uu[64], vv[64];
    unsigned int col[64];
    int seg_page[3], seg_nq[3], nseg = 0;
    int n = 0, n_lin = 0, n_lamp = 0, n_portal = 0;

    /* +lateral = LEFT of travel: point at lateral t off node = (x + tz*t, y,
     * z - tx*t), the same frame the road/gore/sidewalk use. */
    #define TG_TUN_PUSH(ND, T, YY, U, V, C) do {                 \
        px[n] = (ND)->x + (ND)->tz * (T); py[n] = (YY);          \
        pz[n] = (ND)->z - (ND)->tx * (T);                        \
        uu[n] = (U); vv[n] = (V); col[n] = (C); n++;             \
    } while (0)

    /* Wall/roof segment [si, si+1]; only when si+1 is bored too, so a run's last
     * span does not push the tube one span past its mouth. Vertices are grouped
     * by texture page -- all LINING quads, then all LAMP quads, then the PORTAL
     * lintel -- because tg_write_quad_mesh_col draws each command's quads from a
     * contiguous vertex run. */
    if (tg_span_in_tunnel(si + 1)) {
        const TG_Node *a = &nl->v[si];
        const TG_Node *c = &nl->v[si + 1];
        double ha, sa, hc, sc, dx, dz, seglen, u1, vtop, wv;
        double al, ar, cl, cr;
        tg_tunnel_bore(nl, si,     &ha, &sa);
        tg_tunnel_bore(nl, si + 1, &hc, &sc);
        al = sa + ha; ar = sa - ha;          /* inner edges at a (L, R) */
        cl = sc + hc; cr = sc - hc;          /* inner edges at c (L, R) */
        dx = c->x - a->x; dz = c->z - a->z;
        seglen = sqrt(dx * dx + dz * dz);
        u1   = (seglen > 1.0) ? seglen / tile : 0.5;
        vtop = height / tile;
        wv   = (2.0 * ha) / tile;
        /* Left wall inner face */
        TG_TUN_PUSH(a, al, a->y,          0.0, 0.0,  dim);
        TG_TUN_PUSH(c, cl, c->y,          u1,  0.0,  dim);
        TG_TUN_PUSH(c, cl, c->y + height, u1,  vtop, dim);
        TG_TUN_PUSH(a, al, a->y + height, 0.0, vtop, dim);
        /* Right wall inner face */
        TG_TUN_PUSH(a, ar, a->y,          0.0, 0.0,  dim);
        TG_TUN_PUSH(c, cr, c->y,          u1,  0.0,  dim);
        TG_TUN_PUSH(c, cr, c->y + height, u1,  vtop, dim);
        TG_TUN_PUSH(a, ar, a->y + height, 0.0, vtop, dim);
        /* Roof: near-left, far-left, far-right, near-right at ceiling height */
        TG_TUN_PUSH(a, al, a->y + height, 0.0, 0.0, dim);
        TG_TUN_PUSH(c, cl, c->y + height, u1,  0.0, dim);
        TG_TUN_PUSH(c, cr, c->y + height, u1,  wv,  dim);
        TG_TUN_PUSH(a, ar, a->y + height, 0.0, wv,  dim);
        n_lin = n;

        /* [item 8d] Emissive lamp strip near the top of each wall, inset a little
         * toward the bore so it does not z-fight the lining. One quad per side per
         * segment = a continuous run of side lights down the bore. */
        if (lamps_on) {
            const double ins = 40.0;                 /* lateral inset from wall */
            const double ly0 = 0.70 * height, ly1 = 0.80 * height;
            /* Left wall lamp */
            TG_TUN_PUSH(a, al - ins, a->y + ly0, 0.0, 1.0, lampc);
            TG_TUN_PUSH(c, cl - ins, c->y + ly0, u1,  1.0, lampc);
            TG_TUN_PUSH(c, cl - ins, c->y + ly1, u1,  0.0, lampc);
            TG_TUN_PUSH(a, al - ins, a->y + ly1, 0.0, 0.0, lampc);
            /* Right wall lamp */
            TG_TUN_PUSH(a, ar + ins, a->y + ly0, 0.0, 1.0, lampc);
            TG_TUN_PUSH(c, cr + ins, c->y + ly0, u1,  1.0, lampc);
            TG_TUN_PUSH(c, cr + ins, c->y + ly1, u1,  0.0, lampc);
            TG_TUN_PUSH(a, ar + ins, a->y + ly1, 0.0, 0.0, lampc);
            n_lamp = n - n_lin;
        }
    }

    /* [R7 item 9] Portal FACADE across a run mouth (near end at the first bored
     * node, far end at the last). The R6 portal was a single thin lintel beam, so
     * the entrance read as "a weird grey texture, not a tunnel entrance". Build a
     * proper concrete FRAME instead: a HEADER above the opening plus a JAMB down
     * each side, on the R7 concrete-portal page, framing the dark bore on three
     * sides. Each face is double-sided (front quad + reversed quad) so it reads
     * from both directions -- one emit serves both the entrance (faces the
     * approach) and the exit (faces the departure). UVs map the face's TOP edge
     * to page V=0 so the coping band sits along the top; U tiles by world lateral.
     * TD5RE_AUTOTRACK_R7_PORTAL=0 falls back to the R6 lintel (page + winding). */
    if (!tg_span_in_tunnel(si - 1) || !tg_span_in_tunnel(si + 1)) {
        double ph, psh, lo, ro;
        tg_tunnel_bore(nl, si, &ph, &psh);
        lo = psh + ph + wall_t;              /* left opening edge  */
        ro = psh - ph - wall_t;              /* right opening edge */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_R7_PORTAL")) {
            const double JW  = 1400.0;       /* jamb width beyond the opening */
            const double HDR = 1500.0;       /* header height above the opening */
            const double fl  = lo + JW;      /* frame outer left  */
            const double fr  = ro - JW;      /* frame outer right */
            const double yb  = nd->y;                    /* facade foot (road)   */
            const double hb  = nd->y + height;           /* opening top          */
            const double yt  = nd->y + height + HDR;     /* header top (coping)  */
            /* Stand the headwall PROUD of the cutting: the Group-C mountain mass
             * (crown slab + buttresses, TD5_TG_PAGE_HILL) sits AT the mouth node
             * and would occlude a facade drawn in the node plane -- the R7 frame
             * has to project OUTWARD along the road, out past the buttress depth
             * (hz 1400), so it reads as a projecting concrete portal with the bore
             * recessed behind it. Outward = away from the tunnel interior: at the
             * entrance the interior is ahead (+tangent) so we step back, at the
             * exit it is behind so we step forward. */
            const double outsign = tg_span_in_tunnel(si + 1) ? -1.0 : 1.0;
            const double proud   = 1600.0;
            TG_Node fn = *nd;
            fn.x = nd->x + nd->tx * proud * outsign;
            fn.z = nd->z + nd->tz * proud * outsign;
            /* One frame face: laterals [la..lb], y [ylo..yhi]; front + reverse. */
            #define TG_PORTAL_FACE(la, lb, ylo, yhi) do {                        \
                TG_TUN_PUSH(&fn,(la),(yhi), (la)/tile,0.0,            0xFFFFFFFFu);\
                TG_TUN_PUSH(&fn,(lb),(yhi), (lb)/tile,0.0,            0xFFFFFFFFu);\
                TG_TUN_PUSH(&fn,(lb),(ylo), (lb)/tile,((yt)-(ylo))/tile,0xFFFFFFFFu);\
                TG_TUN_PUSH(&fn,(la),(ylo), (la)/tile,((yt)-(ylo))/tile,0xFFFFFFFFu);\
                TG_TUN_PUSH(&fn,(la),(yhi), (la)/tile,0.0,            0xFFFFFFFFu);\
                TG_TUN_PUSH(&fn,(la),(ylo), (la)/tile,((yt)-(ylo))/tile,0xFFFFFFFFu);\
                TG_TUN_PUSH(&fn,(lb),(ylo), (lb)/tile,((yt)-(ylo))/tile,0xFFFFFFFFu);\
                TG_TUN_PUSH(&fn,(lb),(yhi), (lb)/tile,0.0,            0xFFFFFFFFu);\
            } while (0)
            TG_PORTAL_FACE(fl, fr, hb, yt);      /* header over the opening */
            TG_PORTAL_FACE(fl, lo, yb, yt);      /* left jamb  */
            TG_PORTAL_FACE(ro, fr, yb, yt);      /* right jamb */
            #undef TG_PORTAL_FACE
            tg_acct(TG_ACCT_R7_BRIDGE, si);
        } else {
            const double y0 = nd->y + height, y1 = y0 + 500.0;
            const double pw = (lo - ro) / tile;
            TG_TUN_PUSH(nd, lo, y0, 0.0, 0.0,  0xFFFFFFFFu);
            TG_TUN_PUSH(nd, ro, y0, pw,  0.0,  0xFFFFFFFFu);
            TG_TUN_PUSH(nd, ro, y1, pw,  0.17, 0xFFFFFFFFu);
            TG_TUN_PUSH(nd, lo, y1, 0.0, 0.17, 0xFFFFFFFFu);
        }
        n_portal = n - n_lin - n_lamp;
    }
    #undef TG_TUN_PUSH

    if (n <= 0) return 1;
    if (n_lin    > 0) { seg_page[nseg] = lining;    seg_nq[nseg] = n_lin / 4;    nseg++; }
    if (n_lamp   > 0) { seg_page[nseg] = lamp_pg;   seg_nq[nseg] = n_lamp / 4;   nseg++; }
    if (n_portal > 0) { seg_page[nseg] = portal_pg; seg_nq[nseg] = n_portal / 4; nseg++; }
    tg_acct(TG_ACCT_TUNNEL, si);
    tg_acct(TG_ACCT_R6_TUNNEL, si);
    (*added)++;
    return tg_write_quad_mesh_col(blk, px, py, pz, uu, vv, col, n,
                                  seg_page, seg_nq, nseg);
}

/* Tunnel cross-section at span si: two side walls plus a roof, each its own
 * mesh so per-mesh frustum culling cannot pop the whole tunnel at once. The
 * enclosure is drawn DIM (vertex colour) so the interior reads as shadowed --
 * the engine has no interior lighting, so this fakes it. At the run ends a
 * PORTAL frame (a lintel above the road spanning the full mouth) marks the
 * entrance/exit so the near end does not read as an open box.
 *
 * All pieces sample TD5_TG_PAGE_TUNNEL. They used to sample TD5_TG_PAGE_WALL,
 * which is the city FACADE page -- with TD5RE_AUTOTRACK_REAL_TEX on that is a
 * photographic office frontage, so the inside of every tunnel read as building
 * windows. The bore has its own concrete lining page for exactly this reason.
 *
 * Port-original: shipped TD5 has no true tunnels, so there is no reference to
 * match -- this is an invented enclosed section. */
static int tg_emit_tunnel(const TG_NodeList *nl, int si, TG_Buf *blk,
                          int *added)
{
    const TG_Node *n = &nl->v[si];
    const double wall_t = TD5_TG_TUNNEL_WALL_T;
    const double height = TD5_TG_TUNNEL_HEIGHT;
    const double lx = n->tz, lz = -n->tx;
    const unsigned int dim = 0xFF585868u;   /* shadowed blue-grey interior */
    const int lining = tg_tunnel_lining_page(si);   /* item 16a: per-run variety */
    double bore_half, bore_shift, side_x, cx, cz;
    int i;

    if (tg_r5_tunnel_sweep())               /* [R5 item 9] swept, road-following */
        return tg_emit_tunnel_swept(nl, si, blk, added);

    tg_tunnel_bore(nl, si, &bore_half, &bore_shift);
    side_x = bore_half + wall_t;
    /* Bore centreline: the road node displaced laterally onto the bore centre. */
    cx = n->x + lx * bore_shift;
    cz = n->z + lz * bore_shift;

    for (i = 0; i < 3; i++) {
        double ox = 0.0, oz = 0.0, hx, hy, cy;
        if (i < 2) {                                /* side walls */
            double sgn = i ? 1.0 : -1.0;
            ox = lx * side_x * sgn;
            oz = lz * side_x * sgn;
            hx = wall_t; hy = height * 0.5; cy = n->y + height * 0.5;
        } else {                                    /* roof */
            hx = side_x + wall_t; hy = 200.0; cy = n->y + height + 200.0;
        }
        if (!tg_emit_box_mesh(blk, cx + ox, cy, cz + oz,
                              hx, hy, 780.0, n->tx, n->tz,
                              lining, 3000.0, dim))
            return 0;
        (*added)++;
        tg_acct(TG_ACCT_TUNNEL, si);
    }

    /* Portal frame at a run END: a lit lintel across the mouth, above the road,
     * so the entrance/exit reads as a tunnel opening rather than an open box. */
    if (!tg_span_in_tunnel(si - 1) || !tg_span_in_tunnel(si + 1)) {
        if (!tg_emit_box_mesh(blk, cx, n->y + height + 150.0, cz,
                              side_x + wall_t, 500.0, wall_t,
                              n->tx, n->tz, lining, 2000.0,
                              0xFFFFFFFFu))
            return 0;
        (*added)++;
        tg_acct(TG_ACCT_TUNNEL, si);
    }
    return 1;
}

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

/* The terrain line this span's elevation is judged against.
 *
 * This was previously the GLOBAL minimum elevation of the whole track, which
 * made "elevated" mean "higher than the single lowest point in 1800 spans" --
 * true for 87% of a rolling track, so bridge decks were emitted almost
 * everywhere and the guardrail gate inherited the same fault.
 *
 * Instead take the CHORD between the two window endpoints. That is
 * grade-invariant by construction: on any constant grade the midpoint of the
 * endpoints equals this node's own height, so the lift is exactly zero no
 * matter how steep the hill. Only local CONVEXITY -- the road humping up over
 * ground that falls away on both sides, which is what a bridge actually is --
 * produces a positive lift. */
static double tg_local_ground_y(const TG_NodeList *nl, int si)
{
    const int w = td5_env_int("TD5RE_AUTOTRACK_LIFT_WINDOW",
                              TD5_TG_LIFT_WINDOW, 1, 200);
    int a = si - w;
    int b = si + w;
    if (a < 0) a = 0;
    if (b > nl->count - 1) b = nl->count - 1;
    return (nl->v[a].y + nl->v[b].y) * 0.5;
}

/* The span range of the bridge run containing si. Partitioned exactly the way
 * tg_apply_elevation partitions it (fixed RUN-sized blocks from span 0), so the
 * deck hump, the river level and the gorge all describe the same crossing. */
static void tg_bridge_run_bounds(const TG_NodeList *nl, int si, int *s0, int *s1)
{
    int a = (si / TD5_TG_BRIDGE_RUN) * TD5_TG_BRIDGE_RUN;
    int b = a + TD5_TG_BRIDGE_RUN - 1;
    if (b > nl->count - 1) b = nl->count - 1;
    if (a > b) a = b;
    *s0 = a; *s1 = b;
}

/* The LOWEST road node in the bridge run containing si.
 *
 * The one determined height for the crossing. Everything that has to sit UNDER
 * the deck -- the river, the pier tops -- measures from this instead of from its
 * own span's y, and because it is the run MINIMUM the result is at or below the
 * road at every span in the run by construction. Nothing derived from it can
 * surface through the deck no matter what the profile does in between.
 *
 * Off a deliberate run there is no crossing to be level with (the lift-triggered
 * path is a single humped span), so fall back to the span's own height rather
 * than to the minimum of an arbitrary 24-span block, which could be far below. */
static double tg_bridge_deck_y(const TG_NodeList *nl, int si)
{
    int s0, s1, k;
    double lo;

    if (!tg_span_in_bridge_run(si)) return nl->v[si].y;
    tg_bridge_run_bounds(nl, si, &s0, &s1);
    lo = nl->v[s0].y;
    for (k = s0 + 1; k <= s1; k++)
        if (nl->v[k].y < lo) lo = nl->v[k].y;
    return lo;
}

/* River level under a bridge deck at span si (the banks drop to here).
 *
 * Flat over the whole run: this used to be nl->v[si].y - CHASM, i.e. the river
 * surface followed the deck's raised-cosine hump, so the water arched up under
 * the middle of the bridge.
 *
 * The chord MIDPOINT that replaced it was still not safe, and is half of the
 * "water on top of the tunnel" report. A midpoint is only guaranteed to be below
 * the road at the two endpoints it is taken from; anywhere the run's interior
 * dips under its own chord -- or where the 24-span window spills past the main
 * ring onto appended branch-corridor nodes, whose y has nothing to do with this
 * crossing -- the river surfaces above the local road, and a bore anchored to
 * that same local road is then under water. Same class of error as the
 * 2026-08-24 elevated-bridge gate, one level up: a value taken from two samples
 * where it had to hold over the whole run.
 *
 * The run MINIMUM (tg_bridge_deck_y) is that guarantee, and it is the rule
 * tg_sea_level_y already uses for the sea for exactly the same reason. */
static double tg_bridge_water_y(const TG_NodeList *nl, int si)
{
    /* [R6 item 14] A bridge over a WATER biome crosses the SEA, so its water has
     * to sit at the SAME level as the sea beyond the run. The deep dedicated
     * "river" plane sat ~1000 units BELOW that sea (deck - CHASM vs biome-min -
     * WATER_DROP), so where the two met at the run ends the surface stepped down
     * -- half of "the water is not continuous". Matching sea level makes the
     * water one continuous body and drags the gorge bed and the pier feet with it
     * (both derive from here). Over a DRY biome there is no sea to match, so keep
     * the deep gorge river. */
    if (k_biomes[tg_biome_for_span(si)].water)
        return tg_sea_level_y(nl, si);
    return tg_bridge_deck_y(nl, si) - TD5_TG_BRIDGE_CHASM - 300.0;
}

/* How fully span si is "over the gorge", 0 at both ends of the bridge run and 1
 * at the crown, as a raised cosine -- the same shape as the deck hump.
 *
 * Why a ramp and not a flag: the banks beside a bridge span sit far lower than
 * ordinary terrain, so switching that on at a run boundary would leave exactly
 * the kind of unpainted vertical riser this batch is fixing. Ramping the gorge
 * in and out means the terrain is continuous at the run ends by construction. */
static double tg_bridge_gorge_phase(const TG_NodeList *nl, int si)
{
    int s0, s1;
    double u;
    if (!tg_span_in_bridge_run(si)) return 0.0;
    tg_bridge_run_bounds(nl, si, &s0, &s1);
    if (s1 <= s0) return 0.0;
    u = (double)(si - s0) / (double)(s1 - s0);
    return 0.5 - 0.5 * cos(2.0 * TD5_TG_PI * u);
}

/* Visible SUPPORT STRUCTURE under and above the deck (towers, twin piers,
 * cross-braces, under-deck girder). Default ON; TD5RE_AUTOTRACK_BRIDGE_STRUCT=0
 * leaves only the parapets and the single centre pier the deck had before. */
static int tg_bridge_struct_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_STRUCT");
}

/* Page for everything STRUCTURAL on a bridge -- girder, piers, braces, towers.
 *
 * Reported: "bridge pillars should have a different texture than bridge
 * fences". They shared TD5_TG_PAGE_WALL, which is the city FACADE page: with
 * TD5RE_AUTOTRACK_REAL_TEX on the piers were a photographic office frontage,
 * and the parapet immediately above them was the same frontage, so deck and
 * supports read as one undifferentiated slab.
 *
 * [R4 item 18] It shared TD5_TG_PAGE_TUNNEL, the bore LINING page whose damp/
 * soot blotches are meant to read in a dim enclosed section. On a bright
 * EXTERIOR tower those blotches read as a checkerboard -- the "pillar looks
 * wrong and has wrong texture" report. TD5_TG_PAGE_R4_PIER is smooth cast
 * concrete authored for a lit surface. TD5RE_AUTOTRACK_BRIDGE_PIER_TEX=0 puts
 * the structure back on the city FACADE page for comparison. */
static int tg_bridge_pier_page(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_PIER_TEX")
         ? TD5_TG_PAGE_R4_PIER : TD5_TG_PAGE_WALL;
}

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
static int tg_bridge_style(int si)
{
    unsigned int h;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_VARIETY")) return 0;
    h = (unsigned)(si / TD5_TG_BRIDGE_RUN) * 2654435761u;
    return (int)((h >> 12) % (unsigned)TD5_TG_BRIDGE_STYLES);
}

/* Structural page for a given style. Concrete keeps the R4 cast-concrete page;
 * steel and masonry get their own R6 pages so the piers read differently, not
 * just at a different size. Honours the same PIER_TEX A/B switch. */
static int tg_bridge_pier_page_for(int style)
{
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_PIER_TEX"))
        return TD5_TG_PAGE_WALL;
    if (style == 1) return TD5_TG_PAGE_R6_BRIDGE + 1;   /* steel   */
    if (style == 2) return TD5_TG_PAGE_R6_BRIDGE + 2;   /* masonry */
    return TD5_TG_PAGE_R4_PIER;                          /* concrete*/
}

/* Pier PITCH (spans between piers) for a style: steel viaducts stand on a
 * tighter grid, masonry arches on a wider one. */
static int tg_bridge_pier_pitch(int style)
{
    if (style == 1) return 3;
    if (style == 2) return 6;
    return 4;
}

/* Clearance from the deck SURFACE down to the top of anything hanging under it.
 * Derived, not chosen: the girder is a 170 half-height box centred 260 below the
 * road, so its underside is at road - 430. 480 clears that with a margin, which
 * is why a pier topped out here can never break the road surface. */
#define TD5_TG_BRIDGE_UNDER 480.0

/* Bridge: a deck over a river. The road STRIP is the deck; here we add the
 * parapets (side walls) that read as a bridge from the car, plus the structure
 * that carries it. The ground beside a bridge run drops away into the gorge
 * (tg_emit_ground) and the river plane is emitted separately.
 *
 * WHY the extra boxes: with only parapets and one slim centre pier, a deck seen
 * from the side or from underneath read as a floating slab -- the feedback item.
 * Real crossings of this kind (Golden Gate, Sydney Harbour) are legible because
 * of three things, and each maps to one piece here:
 *   - a continuous girder under the deck, so the underside has depth;
 *   - PAIRED piers at the deck edges with a cross-brace between them, not a
 *     single post on the centreline;
 *   - a tower pair rising ABOVE the deck at the crossing's middle, braced
 *     across the top, which is the silhouette that says "bridge" at distance.
 * All pieces are boxes, which matters: tg_emit_models recovers each piece's
 * offset by dividing the appended bytes by the piece count, and that only holds
 * while every piece is the same size. Do not mix a non-box mesh in here. */
static int tg_emit_bridge(const TG_NodeList *nl, int si,
                          TG_Buf *blk, int *added)
{
    const TG_Node *n = &nl->v[si];
    const double ref  = tg_local_ground_y(nl, si);
    const double lift = n->y - ref;
    const int deliberate = tg_span_in_bridge_run(si);
    const double half = n->width * 0.5;
    const int style = tg_bridge_style(si);            /* [R6 item 17] */
    const int pier_page = tg_bridge_pier_page_for(style);
    const int pitch = tg_bridge_pier_pitch(style);
    /* [R7 item 16] "The texture of the pillar doesn't seem fine." The pier boxes
     * tiled every 3000 world units, but a pier LEG is only ~400 wide, so a side
     * face sampled just U 0..0.13 of the page -- a thin vertical sliver that
     * flattened the I-beam / ashlar / form-board pattern into featureless grey.
     * A ~600-unit repeat maps close to one full page WIDTH across a leg (so the
     * flanges land at the leg edges and the web fills the middle) and tiles the
     * detail up the column. Only the vertical members use it; the girder stays at
     * 3000 (it is a wide flat beam along the road). TD5RE_AUTOTRACK_R7_PIERTEX=0
     * restores the 3000 sliver for an A/B. */
    const double pier_tile = td5_env_flag_on("TD5RE_AUTOTRACK_R7_PIERTEX")
                           ? 600.0 : 3000.0;
    /* Deck level for the boxes: the strip surface, with the girder just under. */
    const double wy = deliberate ? tg_bridge_water_y(nl, si) : ref;
    int s0, s1, s;

    if (!deliberate && lift < TD5_TG_BRIDGE_MIN_LIFT) return 1;
    tg_bridge_run_bounds(nl, si, &s0, &s1);

    /* NOTE the parapets are NOT emitted here. [R3 item 12] the old per-span
     * parapet BOX sat level at its own node height, so on the humped deck and
     * its approach ramps the boxes stepped up in a staircase -- the "guardrails
     * look like steps" report. A level box cannot follow a slope, so the rail
     * moved to tg_emit_bridge_rails, which draws a SLOPED ribbon whose top edge
     * runs node-to-node and therefore inclines with the deck. It has to be a
     * separate quad mesh with its own recorded offset because everything emitted
     * in THIS function is a same-byte-size box, recovered by the caller dividing
     * the appended bytes by the box count (see the header comment). */

    if (tg_bridge_struct_enabled()) {
        /* Girder: full-width beam immediately under the deck, one per span, so
         * the underside is a structural depth and not a paper-thin surface.
         * 780 along the road = the same half-length the tunnel sections use, so
         * consecutive spans butt together into a continuous beam. */
        if (!tg_emit_box_mesh(blk, n->x, n->y - 260.0, n->z,
                              half + 60.0, 170.0, 780.0,
                              n->tx, n->tz, pier_page, 3000.0, 0xFFD8D8D8u))
            return 0;
        (*added)++;
        tg_acct(TG_ACCT_BRIDGE, si);
    }

    /* Piers on the style's PITCH (item 17), dropping from the deck underside to
     * the river bed (item 12).
     *
     * Reported: "bridge road should have a predetermined height, this will make
     * pillars not visible on the road surface during bridges". Root cause was
     * arithmetic, and it was exact rather than marginal: the pier was a box of
     * half-height h centred at n->y - h, so its TOP FACE landed at n->y -- the
     * road surface itself, coplanar with the deck. Every pier was drawn into the
     * carriageway, z-fighting the tarmac it was supposed to be holding up. See
     * the item-12 note below for the height fix. */
    if ((si % pitch) == 0) {
        /* [R6 item 12] Piers span the LOCAL deck underside down INTO the bed.
         *
         * Was: top = run-minimum deck (tg_bridge_deck_y) - UNDER, bottom = water
         * level. Two faults, both visible from the side (frame-verified): at the
         * crown the deck sits up to BRIDGE_HEIGHT above the run minimum, so a pier
         * topped out at the run min hung ~3000 units BELOW the deck underside --
         * disconnected posts floating in the gorge, not supports. And the bottom
         * stopped at the water SURFACE, 150 above the gorge bed, so it never
         * reached the floor ("pillars are not reaching the floor").
         *
         * Now the top follows n->y (this span's carriageway) less UNDER, so it is
         * always just below the local deck -- meets the girder at every span,
         * crown included, and still can't break the road. The bottom drops to
         * wy - 400, past the water surface and below the gorge bed (bed sits at
         * wy - 150), so the pier visibly plants on the floor. */
        const double top = n->y - TD5_TG_BRIDGE_UNDER;
        const double bot = wy - 400.0;
        double h = (top - bot) * 0.5;
        double cy;
        if (h < 150.0) h = 150.0;
        cy = top - h;
        if (td5_env_flag_off("TD5RE_BRIDGE_DIAG"))
            TD5_LOG_I(LOG_TAG,
                "BRIDGEDIAG si=%d ny=%.0f deckmin=%.0f wy=%.0f "
                "briverSurf=%.0f sea=%.0f pier_top=%.0f pier_bot=%.0f "
                "gorge_bed_crown=%.0f",
                si, n->y, tg_bridge_deck_y(nl, si), wy,
                tg_bridge_water_y(nl, si) + 100.0, tg_sea_level_y(nl, si),
                top, cy - h, tg_bridge_water_y(nl, si) - 150.0);
        if (tg_bridge_struct_enabled() && style == 2) {
            /* [R6 item 17] MASONRY: one FAT pier on a wide pitch, with a shallow
             * arch cap under the deck that reads as the springing of a stone
             * arch -- no leg pair, no towers (added below only for 0/1). */
            if (!tg_emit_box_mesh(blk, n->x, cy, n->z,
                                  620.0, h, 620.0, n->tx, n->tz,
                                  pier_page, pier_tile, 0xFFFFFFFFu))
                return 0;
            (*added)++;
            tg_acct(TG_ACCT_BRIDGE, si);
            if (!tg_emit_box_mesh(blk, n->x, top - 140.0, n->z,
                                  half * 0.9, 170.0, 360.0, n->tx, n->tz,
                                  pier_page, pier_tile, 0xFFFFFFFFu))
                return 0;
            (*added)++;
            tg_acct(TG_ACCT_BRIDGE, si);
        } else if (tg_bridge_struct_enabled()) {
            /* [R6 item 17] Paired legs under the deck EDGES. STEEL (style 1) legs
             * are slimmer, set wider and get a SECOND (upper) cross-brace so the
             * pier reads as a braced steel bent; CONCRETE (style 0) legs are
             * stockier with a single brace. */
            const double legw   = (style == 1) ? 200.0 : 300.0;
            const double legpos = (style == 1) ? 0.85 : 0.70;
            for (s = 0; s < 2; s++) {
                double side = s ? 1.0 : -1.0;
                double lx = n->tz * side, lz = -n->tx * side;
                if (!tg_emit_box_mesh(blk, n->x + lx * (half * legpos),
                                      cy, n->z + lz * (half * legpos),
                                      legw, h, legw, n->tx, n->tz,
                                      pier_page, pier_tile, 0xFFFFFFFFu))
                    return 0;
                (*added)++;
                tg_acct(TG_ACCT_BRIDGE, si);
            }
            /* Brace across the legs, a third of the way down the pier. */
            if (!tg_emit_box_mesh(blk, n->x, top - h * 0.55, n->z,
                                  half * legpos + legw, 130.0, 200.0,
                                  n->tx, n->tz, pier_page, pier_tile,
                                  0xFFFFFFFFu))
                return 0;
            (*added)++;
            tg_acct(TG_ACCT_BRIDGE, si);
            if (style == 1) {                 /* second, upper brace for steel */
                if (!tg_emit_box_mesh(blk, n->x, top - h * 0.22, n->z,
                                      half * legpos + legw, 110.0, 180.0,
                                      n->tx, n->tz, pier_page, pier_tile,
                                      0xFFFFFFFFu))
                    return 0;
                (*added)++;
                tg_acct(TG_ACCT_BRIDGE, si);
            }
        } else {
            if (!tg_emit_box_mesh(blk, n->x, cy, n->z,
                                  450.0, h, 450.0,
                                  n->tx, n->tz, pier_page, pier_tile,
                                  0xFFFFFFFFu))
                return 0;
            (*added)++;
            tg_acct(TG_ACCT_BRIDGE, si);
        }
    }

    /* Towers: once per crossing, at the crown span, so a run has ONE gateway
     * rather than a forest of posts. [R6 item 17] CONCRETE (0) gets a stocky
     * gateway; STEEL (1) a slimmer, taller pylon pair; MASONRY (2) has NO towers
     * (a stone viaduct is arches all the way, not a gateway) -- so the three
     * crossings on a track read differently at distance. */
    if (deliberate && tg_bridge_struct_enabled() && style != 2 &&
        si == s0 + (s1 - s0) / 2) {
        const double th = (style == 1) ? 2200.0 : 1700.0;  /* tower half-height */
        const double tw = (style == 1) ? 170.0 : 240.0;    /* tower half-width  */
        for (s = 0; s < 2; s++) {
            double side = s ? 1.0 : -1.0;
            double lx = n->tz * side, lz = -n->tx * side;
            if (!tg_emit_box_mesh(blk, n->x + lx * (half + 300.0),
                                  n->y + th, n->z + lz * (half + 300.0),
                                  tw, th, tw, n->tx, n->tz,
                                  pier_page, pier_tile, 0xFFFFFFFFu))
                return 0;
            (*added)++;
            tg_acct(TG_ACCT_BRIDGE, si);
        }
        /* Cross-member near the top of the towers, closing the gateway. */
        if (!tg_emit_box_mesh(blk, n->x, n->y + th * 1.7, n->z,
                              half + 540.0, 160.0, 200.0,
                              n->tx, n->tz, pier_page, pier_tile,
                              0xFFFFFFFFu))
            return 0;
        (*added)++;
        tg_acct(TG_ACCT_BRIDGE, si);
    }
    return 1;
}

/* Does span si carry a bridge DECK? The same two-way test tg_emit_bridge makes
 * internally (a deliberate run, OR a lone span lifted above the local terrain),
 * factored out so the parapet emitter gates on EXACTLY the spans that got a
 * deck -- a rail on a span with no deck under it would float. */
static int tg_span_is_bridge_deck(const TG_NodeList *nl, int si)
{
    double lift;
    if (tg_span_in_bridge_run(si)) return 1;
    lift = nl->v[si].y - tg_local_ground_y(nl, si);
    return lift >= TD5_TG_BRIDGE_MIN_LIFT;
}

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

/* [R3 item 13] Overhead ribs across the deck are OPTIONAL structure above the
 * road. Only drawn when the support structure is on; default ON so the fuller
 * "there should be structure above" silhouette is what ships. Set
 * TD5RE_AUTOTRACK_BRIDGE_OVERHEAD=0 to leave just the deck, rails and towers. */
static int tg_bridge_overhead_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_OVERHEAD");
}

/* One sloped parapet panel spanning node si -> si+1 on one deck edge. This is
 * the item-12 fix: the top edge runs from (x0,y0) to (x1,y1), so it INCLINES
 * with the deck instead of the old level box that stepped. A single quad, its
 * own recorded mesh offset (it cannot join tg_emit_bridge's equal-size box
 * group). Sampled from the barrier page like the old parapet. */
static int tg_emit_bridge_rail_panel(TG_Buf *m,
                                     double x0, double y0, double z0,
                                     double x1, double y1, double z1,
                                     int si, size_t *moff, int *pn)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    const double base = TD5_TG_BRIDGE_RAIL_BASE, h = TD5_TG_BRIDGE_RAIL_H;
    /* [R4 item 16b] Was TD5_TG_PAGE_RAIL, an armco authored for a HORIZONTAL
     * strip. This panel maps u across the panel HEIGHT and v along the road, so
     * that page sampled rotated and "is not a guardrail texture". The R4 page is
     * drawn in these axes -- rails run horizontally, posts periodically along.
     * [R5 item 17a] The R4 page still read as a CONCRETE barrier, not a
     * guardrail; the R5 armco was steel but still an OPAQUE slab.
     * [R6 item 13] A guardrail with no extrusion has to be ALPHA-KEYED, not a
     * solid panel -- real armco is mostly air. Default to the R6 W-beam page
     * (type 1, transparent between beam and posts). TD5RE_AUTOTRACK_ARMCO=0
     * restores the R5 opaque steel face for an A/B. */
    int seg_page = td5_env_flag_on("TD5RE_AUTOTRACK_ARMCO")
                 ? TD5_TG_PAGE_R6_BRIDGE + 0 : TD5_TG_PAGE_R5_BRIDGE + 0;
    int seg_nq = 1;

    /* Quad: near-bottom, far-bottom, far-top, near-top. u across the page over
     * the panel height (0..1 = the whole barrier face); v advances one page per
     * span down the run. */
    px[0] = x0; py[0] = y0 + base;     pz[0] = z0; uu[0] = 0.0; vv[0] = (double)si;
    px[1] = x1; py[1] = y1 + base;     pz[1] = z1; uu[1] = 0.0; vv[1] = (double)(si + 1);
    px[2] = x1; py[2] = y1 + base + h; pz[2] = z1; uu[2] = 1.0; vv[2] = (double)(si + 1);
    px[3] = x0; py[3] = y0 + base + h; pz[3] = z0; uu[3] = 1.0; vv[3] = (double)si;

    moff[*pn] = m->len;
    if (!tg_write_quad_mesh(m, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
        return 0;
    (*pn)++;
    tg_acct(TG_ACCT_BRIDGE, si);
    return 1;
}

/* Parapets on both deck edges (item 12) plus optional overhead ribs (item 13),
 * for span si. Recorded via moff/pn like tg_emit_bridge_water, so each mesh
 * carries its own offset -- these are quads, not the boxes tg_emit_bridge's
 * caller recovers by even division. */
/* [R5 item 17b] One DECK KERB quad along one edge, span si -> si+1, filling the
 * horizontal gap between the carriageway edge and the parapet line.
 *
 * The parapet sits at width/2 + RAIL_OUT (120) but the road mesh only reaches
 * width/2, so between the tarmac and the barrier there was a 120-unit strip of
 * nothing at deck level -- you looked straight down through it to the river.
 * That open strip is the "gap between the guardrails and the road" report. This
 * lays a flat concrete kerb across it at deck level, so the deck reads as solid
 * out to the barrier. Concrete (pier) page, its own recorded offset. */
#define TD5_TG_BRIDGE_RAIL_OUT 120.0
static int tg_emit_bridge_kerb_panel(TG_Buf *m,
                                     double ix0, double iz0, double ox0, double oz0,
                                     double ix1, double iz1, double ox1, double oz1,
                                     double y0, double y1,
                                     int si, size_t *moff, int *pn)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    int seg_page = tg_bridge_pier_page(), seg_nq = 1;

    /* inner-near, inner-far, outer-far, outer-near -- a proper ring. U across
     * the kerb width, V one page per span down the run. */
    px[0] = ix0; py[0] = y0; pz[0] = iz0; uu[0] = 0.0; vv[0] = (double)si;
    px[1] = ix1; py[1] = y1; pz[1] = iz1; uu[1] = 0.0; vv[1] = (double)(si + 1);
    px[2] = ox1; py[2] = y1; pz[2] = oz1; uu[2] = 1.0; vv[2] = (double)(si + 1);
    px[3] = ox0; py[3] = y0; pz[3] = oz0; uu[3] = 1.0; vv[3] = (double)si;

    moff[*pn] = m->len;
    if (!tg_write_quad_mesh(m, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
        return 0;
    (*pn)++;
    tg_acct(TG_ACCT_R5_BRIDGE, si);
    return 1;
}

static int tg_emit_bridge_rails(const TG_NodeList *nl, int si,
                                TG_Buf *m, size_t *moff, int *pn)
{
    const TG_Node *n0 = &nl->v[si];
    const TG_Node *n1;
    double half0, half1, road0, road1;
    const int kerb = td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_KERB");
    int s;

    if (si + 1 >= nl->count) return 1;
    if (!tg_span_is_bridge_deck(nl, si)) return 1;
    n1 = &nl->v[si + 1];
    road0 = n0->width * 0.5;
    road1 = n1->width * 0.5;
    half0 = road0 + TD5_TG_BRIDGE_RAIL_OUT;
    half1 = road1 + TD5_TG_BRIDGE_RAIL_OUT;

    for (s = 0; s < 2; s++) {
        const double side = s ? 1.0 : -1.0;
        const double x0 = n0->x + n0->tz * side * half0;
        const double z0 = n0->z - n0->tx * side * half0;
        const double x1 = n1->x + n1->tz * side * half1;
        const double z1 = n1->z - n1->tx * side * half1;
        /* [R5 item 17b] Kerb first, so the deck is solid out to the barrier. */
        if (kerb) {
            const double ix0 = n0->x + n0->tz * side * road0;
            const double iz0 = n0->z - n0->tx * side * road0;
            const double ix1 = n1->x + n1->tz * side * road1;
            const double iz1 = n1->z - n1->tx * side * road1;
            if (!tg_emit_bridge_kerb_panel(m, ix0, iz0, x0, z0, ix1, iz1, x1, z1,
                                           n0->y, n1->y, si, moff, pn))
                return 0;
        }
        if (!tg_emit_bridge_rail_panel(m, x0, n0->y, z0, x1, n1->y, z1,
                                       si, moff, pn))
            return 0;
    }

    /* Overhead rib, every 6th span. [R4 item 17] The cross-beam alone floated:
     * a bar in the sky with nothing joining it to the deck ("floating element").
     * Give it two VERTICAL LEGS down to the parapet tops so it reads as a portal
     * gantry, not a hovering slab.
     *
     * [R5 item 13] "geometry going over the bridge that doesn't have any depth
     * to it, looks 2D flat": the round-4 beam and legs were single QUADS -- a
     * horizontal slab with no thickness and vertical sheets with no lateral
     * width, so edge-on they vanished and read as flat cut-outs spanning the
     * deck. Build them as solid BOXES instead (tg_emit_box_mesh, six faces each),
     * so the gantry has real depth from every angle. Concrete (pier) page, so it
     * matches the towers. TD5RE_AUTOTRACK_BRIDGE_OVERHEAD=0 removes the gantry. */
    if (tg_bridge_struct_enabled() && tg_bridge_overhead_enabled() &&
        (si % 6) == 0) {
        const int pier_page = tg_bridge_pier_page_for(tg_bridge_style(si));
        const double ry = n0->y + TD5_TG_BRIDGE_RAIL_BASE
                        + TD5_TG_BRIDGE_RAIL_H + 1800.0;   /* clears traffic */
        /* Legs stand from the parapet top up to the beam. */
        const double ly = n0->y + TD5_TG_BRIDGE_RAIL_BASE + TD5_TG_BRIDGE_RAIL_H;
        const double lx = n0->tz, lz = -n0->tx;            /* left unit  */
        const double fwx = n0->tx, fwz = n0->tz;           /* along-road unit */
        const double lex = n0->x + lx * half0, lez = n0->z + lz * half0;
        const double rex = n0->x - lx * half0, rez = n0->z - lz * half0;
        int leg;
        /* Beam: a box spanning both deck edges, 260 tall x 320 deep along road. */
        moff[*pn] = m->len;
        if (!tg_emit_box_mesh(m, n0->x, ry, n0->z, half0, 130.0, 160.0,
                              fwx, fwz, pier_page, 3000.0, 0xFFFFFFFFu))
            return 0;
        (*pn)++;
        tg_acct(TG_ACCT_BRIDGE, si);
        /* Two legs: a solid post at each deck edge, from parapet top to beam. */
        for (leg = 0; leg < 2; leg++) {
            const double ex = leg ? rex : lex, ez = leg ? rez : lez;
            const double cy = (ly + ry) * 0.5, hy = (ry - ly) * 0.5;
            moff[*pn] = m->len;
            if (!tg_emit_box_mesh(m, ex, cy, ez, 150.0, hy, 150.0,
                                  fwx, fwz, pier_page, 3000.0, 0xFFFFFFFFu))
                return 0;
            (*pn)++;
            tg_acct(TG_ACCT_BRIDGE, si);
        }
    }
    return 1;
}

/* The river surface spanning the full width under a bridge run, recording its
 * own mesh offset. ONE quad with world-projected UVs, for the same reason as
 * tg_emit_water: a cell grid seamed and marched, a world projection is
 * continuous across spans and across the two water emitters (same tile size, so
 * a river and a sea meeting at a biome edge stay on one texture grid). */
static int tg_emit_bridge_water(const TG_NodeList *nl, int si,
                                TG_Buf *m, size_t *moff, int *pn)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    const TG_Node *n0 = &nl->v[si];
    const TG_Node *n1;
    double lx, lz, wy;
    const double BW = TD5_TG_BRIDGE_WATER_HALF;
    int i, seg_page = TD5_TG_PAGE_WATER, seg_nq = 1;

    if (si + 1 >= nl->count) return 1;
    if (!tg_water_span_clear(si)) return 1;
    n1 = &nl->v[si + 1];
    lx = n0->tz; lz = -n0->tx;              /* left unit */
    /* [R6 item 14] Over a water biome sit EXACTLY at sea level so the river and
     * the sea beyond the run are coplanar (no step). Over a dry biome the +100
     * keeps the surface just under the banks as before. */
    wy = tg_bridge_water_y(nl, si)
       + (k_biomes[tg_biome_for_span(si)].water ? 0.0 : 100.0);

    px[0] = n0->x - lx * BW; pz[0] = n0->z - lz * BW;
    px[1] = n1->x - lx * BW; pz[1] = n1->z - lz * BW;
    px[2] = n1->x + lx * BW; pz[2] = n1->z + lz * BW;
    px[3] = n0->x + lx * BW; pz[3] = n0->z + lz * BW;
    for (i = 0; i < 4; i++) {
        py[i] = wy;
        uu[i] = px[i] / TD5_TG_WATER_TILE;
        vv[i] = pz[i] / TD5_TG_WATER_TILE;
    }

    moff[*pn] = m->len;
    if (!tg_write_quad_mesh(m, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
        return 0;
    (*pn)++;
    tg_acct_range(TG_ACCT_WATER, si, si + 1);   /* river under a bridge run */
    return 1;
}

/* Lateral half-width of the coastline band. The river is ±BRIDGE_WATER_HALF
 * (32000) but the seam the user sees is the near-deck one; a ±12000 band covers
 * the gorge pull-back (GORGE_INSET 9000) plus a margin without a huge tilted
 * slab reaching to the far water edge. */
#define TD5_TG_COAST_HALF 12000.0

/* [R4 item 20] COASTLINE where the bridge water meets the land, at the two
 * LONGITUDINAL ends of the river rectangle (perpendicular to the bridge). The
 * river is a flat plane under the run; before the run starts and after it ends
 * the terrain is back at road level, so the plane's transverse edge met the bank
 * as an open seam ("grass slopes not connecting properly with water"). This lays
 * one shore band per open end, from the water-edge node at the river surface out
 * to the neighbouring land node at ground level, so water grades into shore into
 * bank. Gated (default ON); TD5RE_AUTOTRACK_COASTLINE=0 removes it.
 *
 * VISUAL CAVEAT: the chase camera never gives a side view of the gorge, so this
 * band's appearance at the junction is NOT frame-verified -- only that it FIRES
 * at the run-boundary spans (element inventory "coastline"). */
static int tg_emit_bridge_coast(const TG_NodeList *nl, int si,
                                TG_Buf *m, size_t *moff, int *pn)
{
    const double CH = TD5_TG_COAST_HALF;
    int s0, s1, ends = 0, k;
    int wnode[2], lnode[2];

    if (!tg_span_in_bridge_run(si)) return 1;
    if (!tg_water_span_clear(si)) return 1;         /* no river here */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_COASTLINE")) return 1;
    tg_bridge_run_bounds(nl, si, &s0, &s1);

    /* River nodes run s0 .. s1+1; its open ends are node s0 (back onto s0-1) and
     * node s1+1 (forward onto s1+2). */
    if (si == s0 && s0 - 1 >= 0) {
        wnode[ends] = s0;     lnode[ends] = s0 - 1;     ends++;
    }
    if (si == s1 && s1 + 2 < nl->count) {
        wnode[ends] = s1 + 1; lnode[ends] = s1 + 2;     ends++;
    }

    for (k = 0; k < ends; k++) {
        const TG_Node *nw = &nl->v[wnode[k]];
        const TG_Node *nn = &nl->v[lnode[k]];
        const double wy = tg_bridge_water_y(nl, si) + 100.0;   /* river surface */
        const double gy = nn->y - 70.0;   /* land at road (== TD5_TG_GROUND_DROP,
                                            * whose #define sits below the GROUND
                                            * block; the 70u z-fight bias). */
        const double lx = nw->tz, lz = -nw->tx;                /* left unit     */
        double px[4], py[4], pz[4], uu[4], vv[4];
        int seg_page = TD5_TG_PAGE_R4_COAST, seg_nq = 1, i;

        px[0] = nw->x - lx * CH; py[0] = wy; pz[0] = nw->z - lz * CH;
        px[1] = nn->x - lx * CH; py[1] = gy; pz[1] = nn->z - lz * CH;
        px[2] = nn->x + lx * CH; py[2] = gy; pz[2] = nn->z + lz * CH;
        px[3] = nw->x + lx * CH; py[3] = wy; pz[3] = nw->z + lz * CH;
        for (i = 0; i < 4; i++) {
            uu[i] = px[i] / TD5_TG_WATER_TILE;
            vv[i] = pz[i] / TD5_TG_WATER_TILE;
        }
        moff[*pn] = m->len;
        if (!tg_write_quad_mesh(m, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
            return 0;
        (*pn)++;
        tg_acct(TG_ACCT_COASTLINE, wnode[k]);
    }
    return 1;
}

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

/* Ground as two SKIRT STRIPS per span -- one off each verge -- built from the
 * same road-edge computation the road mesh uses. That makes the terrain follow
 * width changes, curvature and elevation exactly, and adjacent spans share edge
 * positions so there are no seams.
 *
 * This replaced a flat box slab per display-list entry. A box cannot follow a
 * curving or undulating road: its top face sat at one height and one heading
 * for four spans, so on bends its straight edge cut visibly across the verge
 * and slab-to-slab joins showed.
 */
/* Is this biome a SNOW biome? Derived from the drivable surface rather than
 * carried in TG_Biome: the biome table is shared with several parallel work
 * areas this cycle, and "the road is ice" is already exactly the fact we mean --
 * an icy road with green grass beside it was the feedback item. Kept as its own
 * accessor so a future snow biome with a grippy road only has to change here. */
static int tg_biome_is_snow(const TG_Biome *b)
{
    return b->road_surf == RS_ICE;
}

/* The page the terrain around span si is textured from.
 *
 * Overrides the biome's own ground_page with SNOW on icy biomes -- but NEVER on
 * a tunnel span: a tunnel is an enclosed section (Group C's massing sits around
 * its mouths) and snow inside it would show through the lining as a bright
 * floor. Cross-group constraint, gate kept here so both the skirt and the far
 * terrain read the same page. TD5RE_AUTOTRACK_SNOW=0 restores the green skirt. */
static int tg_ground_page_for_span(int si, const TG_Biome *b)
{
    /* [R3 item 14] "patches of grass on bridges". The terrain beside a bridge
     * run is the GORGE bank -- rock/earth dropping to the river -- not lawn, but
     * it inherited the biome's green ground page (FIELDS/COAST carry bridges), so
     * grass slabs showed under and beside the deck. Give the whole run the
     * neutral concrete/earth embankment page instead. Cosmetic only; the gorge
     * SHAPE (pull-back + drop to the river) is set in tg_ground_side. */
    if (tg_span_in_bridge_run(si)) return TD5_TG_PAGE_GROUND;
    if (!tg_biome_is_snow(b)) return b->ground_page;
    if (tg_span_in_tunnel(si)) return b->ground_page;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_SNOW")) return b->ground_page;
    return TD5_TG_PAGE_SNOW;
}

/* Lateral clearance the RIGHT-hand skirt must leave at span si so it does not
 * cover the branch carriageway.
 *
 * Root cause of "grass on top of the road on a branch": the skirt is built from
 * the MAIN centreline's road edges (tg_road_edge with shift 0, full width), so
 * its inner edge sits at -width/2 -- but over a fork the branch half-carriageway
 * has bowed outward to as far as -(width/2 + BOW*width), i.e. underneath the
 * skirt. Pushing the skirt's inner edge out to the branch's OUTER edge leaves
 * the carriageway uncovered; the gore mesh already fills the wedge inboard of
 * it, so nothing shows through. Returns 0 where no fork is active.
 *
 * This was the second hand-rolled copy of the fork arithmetic (the note in the
 * CARRIAGEWAY QUERY section named it). It read s_forks directly and assumed a
 * FIXED half carriageway (w*0.25 out from the corridor centre), so once the
 * corridor could widen and taper it UNDER-REPORTED and the skirt went back over
 * the branch. Retired onto tg_carriageway_reach at the round-2 merge
 * (2026-08-27).
 *
 * The 0-off-a-fork return is DELIBERATE and is why this does not just call
 * tg_carriageway_clear_gap with a 200 margin: the skirt's inner point sits
 * flush with the asphalt on an ordinary span on purpose (see tg_ground_side --
 * setting it back left a thin void between road and grass). The margin is only
 * wanted where there is actually a carriageway to clear, so the excess over the
 * main road's own half width is what gets tested. */
static double tg_ground_branch_clear(const TG_NodeList *nl, int si)
{
    double over;

    if (!tg_branches_enabled()) return 0.0;
    /* How far the outermost carriageway reaches PAST the main road edge. Zero
     * on any span no corridor bows across, since reach floors at the half width. */
    over = tg_carriageway_reach(nl, si, -1.0) - tg_road_half_width(nl, si);
    if (over <= 0.0) return 0.0;
    return over + 200.0;                 /* + margin, no shared edge */
}

/* One side's terrain CROSS-SECTION at span si: a chain of (distance from the
 * road edge, drop below the road) points, innermost first. Consecutive points
 * make one quad, so a plain verge is 2 points / 1 quad and a beach is 3 points /
 * 2 quads. One function, so the skirt in tg_emit_ground and the far terrain in
 * tg_emit_fb_terrain cannot disagree about where the skirt ended or how low. */
#define TD5_TG_GROUND_MAXPT 3
typedef struct {
    double d[TD5_TG_GROUND_MAXPT];
    double dy[TD5_TG_GROUND_MAXPT];
    int    n;
} TG_GroundProf;

/* [R6 item 6] How far the flat verge (near ground skirt) reaches outward. At the
 * historical 24000 the skirt is a wide, near-flat apron that on a DESCENT
 * projects over the road ahead and hides it -- seed 99991 span 188 (a straight,
 * gently descending 4-lane avenue) vanished behind its own verge. PROVEN by an
 * A/B frame: the road, the car and everything forward reappeared the moment the
 * skirt was pulled in, with far-band and buildings still on. 12000 still reaches
 * well past the sidewalks, and the far background band starts exactly where the
 * skirt ends (tg_ground_side is the far-band's source too), so the backdrop
 * simply comes in with it -- no gap, no bare ground. Uniform, so the per-group
 * far-band samples stay consistent with the per-span skirt. Default ON;
 * TD5RE_AUTOTRACK_VERGE_NARROW=0 restores the old 24000 apron for an A/B. */
static double tg_verge_reach(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_VERGE_NARROW")
         ? TD5_TG_VERGE_REACH : TD5_TG_GROUND_WIDTH;
}

static void tg_ground_side(const TG_NodeList *nl, int si, int is_left,
                           double water_side, TG_GroundProf *p)
{
    const double phase = tg_bridge_gorge_phase(nl, si);
    const int seaward = (water_side > 0.0 && is_left) ||
                        (water_side < 0.0 && !is_left);

    /* Default: flush with the asphalt at the road edge, a gentle embankment
     * outward. The INNER point stays at road level on purpose -- dropping the
     * whole skirt left a thin void/lip between road and grass. */
    p->n = 2;
    p->d[0]  = 0.0;                    p->dy[0] = 0.0;
    p->d[1]  = tg_verge_reach();      p->dy[1] = TD5_TG_GROUND_DROP;

    /* [R4 item 16a] The GORGE wins over the seaward beach on a bridge run.
     * On a COAST bridge (seed 99991 span 1160-1199) the seaward test fired first
     * and laid a FLAT beach verge alongside the raised deck -- a light concrete
     * strip hanging at deck height, which reads as "a bridge with a sidewalk on
     * the left". A raised deck crosses the water; there is no beach beside it, so
     * inside the run (phase > 0) the bank must pull back and drop to the river on
     * BOTH sides. phase is 0 exactly at the run ends, so the beach still applies
     * there and the two treatments stay continuous at the boundary. */
    if (seaward && phase <= 0.0) {
        const double d = nl->v[si].y - tg_sea_level_y(nl, si);
        const double fall = (d > 0.0 ? d : (double)TD5_TG_WATER_DROP)
                          - TD5_TG_GROUND_DROP;
        p->n = 3;
        p->d[1]  = TD5_TG_SHORE_VERGE;  p->dy[1] = TD5_TG_GROUND_DROP;
        p->d[2]  = TD5_TG_SHORE_END;
        p->dy[2] = TD5_TG_GROUND_DROP
                 + fall * (TD5_TG_SHORE_END - TD5_TG_SHORE_VERGE)
                        / ((double)TD5_TG_WATER_BEACH - TD5_TG_SHORE_VERGE);
        return;
    }
    if (phase > 0.0) {
        /* Gorge: the bank pulls back from the deck; the pull-back scales with the
         * run phase so the terrain is continuous with the ordinary skirt at the
         * run ends. `bed` is the R6 drop target (just under the river bed); it is
         * clamped non-negative because a river plane above its road node is not a
         * gorge and a negative drop would lift the terrain above the road. */
        double bed = nl->v[si].y - (tg_bridge_water_y(nl, si) - 150.0);
        if (bed < 0.0) bed = 0.0;
        p->d[0]  = phase * TD5_TG_GORGE_INSET;
        /* [R6 item 14] The OUTER edge used to stay at the ordinary 24000 verge,
         * so once the inner point dropped, the whole skirt was a wide near-flat
         * GROUND (concrete-tile) shelf lying ACROSS the river. Bring the outer
         * edge in to a narrow band at the crown, lerping back to the ordinary
         * verge at the run ends so it stays continuous with the plain skirt. */
        p->d[1]  = TD5_TG_GROUND_WIDTH
                 + phase * (phase * TD5_TG_GORGE_INSET + 3000.0
                            - TD5_TG_GROUND_WIDTH);
        /* [R7 item 17] "Below bridges only water should be rendered", still not
         * true after R6. R6 dropped the skirt to the river BED (wy-150) but ramped
         * there LINEARLY with phase, so across the whole INTERIOR of the run the
         * concrete skirt sat ABOVE the water surface (wy+100) -- the "tiles at one
         * height and water at a lower height on alternated spans" the user still
         * sees. The bed was never the right target: the bridge-water plane
         * (BRIDGE_WATER_HALF 32000) already covers the entire gorge, so anything
         * at or above the water surface pokes through it. Drop the skirt just
         * BELOW the water surface on a FAST ramp (fully submerged by ~30% into the
         * run), so the interior shows only water while the run ENDS (phase -> 0)
         * still rise to the bank as a shore. Both profile points share the depth,
         * so the submerged strip is flat and wholly hidden by the water plane.
         * TD5RE_AUTOTRACK_BRIDGE_SUBMERGE=0 restores the R6 bed ramp for an A/B. */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_SUBMERGE")) {
            const double surf = tg_bridge_water_y(nl, si)
                + (k_biomes[tg_biome_for_span(si)].water ? 0.0 : 100.0);
            double sub = nl->v[si].y - surf + 250.0;   /* 250 below the surface */
            double r   = phase * (1.0 / 0.30);
            if (sub < TD5_TG_GROUND_DROP) sub = TD5_TG_GROUND_DROP;
            if (r > 1.0) r = 1.0;
            p->dy[0] = TD5_TG_GROUND_DROP + r * (sub - TD5_TG_GROUND_DROP);
            p->dy[1] = p->dy[0];
        } else {
            p->dy[0] = phase * bed;
            p->dy[1] = TD5_TG_GROUND_DROP + phase * (bed - TD5_TG_GROUND_DROP);
        }
        return;
    }
    if (!is_left) {
        /* Branch corridor bows into the right verge -- keep off its carriageway. */
        p->d[0] = tg_ground_branch_clear(nl, si);
        if (p->d[0] > p->d[1] - 1000.0)
            p->d[0] = p->d[1] - 1000.0;
    }
}

static int tg_emit_ground(const TG_NodeList *nl, int si, TG_Buf *blk,
                          double water_side)
{
    const TG_Biome *b = &k_biomes[tg_biome_for_span(si)];
    double nlx, nly, nlz, nrx, nry, nrz;   /* near left / right road edge */
    double flx, fly, flz, frx, fry, frz;   /* far  left / right road edge */
    double nux, nuz, fux, fuz;             /* outward lateral units */
    double len;
    /* Up to (MAXPT-1) quads per side. */
    double px[(TD5_TG_GROUND_MAXPT - 1) * 8], py[(TD5_TG_GROUND_MAXPT - 1) * 8];
    double pz[(TD5_TG_GROUND_MAXPT - 1) * 8], uu[(TD5_TG_GROUND_MAXPT - 1) * 8];
    double vv[(TD5_TG_GROUND_MAXPT - 1) * 8];
    int seg_page = tg_ground_page_for_span(si, b), seg_nq;
    int s, k, n = 0;

    tg_road_edge(nl, si, 0.0, 0.0, 1.0, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
    tg_road_edge(nl, si, 1.0, 0.0, 1.0, &flx, &fly, &flz, &frx, &fry, &frz);

    /* Outward direction = along the cross-section, away from the centre. */
    nux = nlx - nrx; nuz = nlz - nrz;
    len = sqrt(nux * nux + nuz * nuz);
    if (len < 1e-6) { nux = 1.0; nuz = 0.0; } else { nux /= len; nuz /= len; }
    fux = flx - frx; fuz = flz - frz;
    len = sqrt(fux * fux + fuz * fuz);
    if (len < 1e-6) { fux = 1.0; fuz = 0.0; } else { fux /= len; fuz /= len; }

    /* One quad per profile segment per side, loop order near-in, near-out,
     * far-out, far-in so each quad is a proper ring.
     *
     * ISOTROPIC UV: V advances one tile per span (~SPAN_LENGTH world units), so U
     * is the OUTWARD DISTANCE in span-lengths, which makes each tile square. U
     * running 0..4 across the whole 24000-unit skirt was a ~4:1 lateral stretch
     * that smeared the texture and defeated the (isotropic, box-filter) mipmaps,
     * so the far ground shimmered. Taking U from the distance also means the
     * tiling does not change when the profile does. */
    for (s = 0; s < 2; s++) {
        const int is_left = s ? 0 : 1;
        const double ox = is_left ? nux : -nux, oz = is_left ? nuz : -nuz;
        const double gx = is_left ? fux : -fux, gz = is_left ? fuz : -fuz;
        const double bnx = is_left ? nlx : nrx, bnz = is_left ? nlz : nrz;
        const double bfx = is_left ? flx : frx, bfz = is_left ? flz : frz;
        const double bny = is_left ? nly : nry, bfy = is_left ? fly : fry;
        TG_GroundProf p;

        tg_ground_side(nl, si, is_left, water_side, &p);
        for (k = 0; k + 1 < p.n; k++) {
            const double d0 = p.d[k],  d1 = p.d[k + 1];
            const double y0 = p.dy[k], y1 = p.dy[k + 1];
            const double u0 = d0 / (double)TD5_TG_SPAN_LENGTH;
            const double u1 = d1 / (double)TD5_TG_SPAN_LENGTH;
            px[n]=bnx+ox*d0; py[n]=bny-y0; pz[n]=bnz+oz*d0;
            uu[n]=u0; vv[n]=(double)si;     n++;
            px[n]=bnx+ox*d1; py[n]=bny-y1; pz[n]=bnz+oz*d1;
            uu[n]=u1; vv[n]=(double)si;     n++;
            px[n]=bfx+gx*d1; py[n]=bfy-y1; pz[n]=bfz+gz*d1;
            uu[n]=u1; vv[n]=(double)si+1.0; n++;
            px[n]=bfx+gx*d0; py[n]=bfy-y0; pz[n]=bfz+gz*d0;
            uu[n]=u0; vv[n]=(double)si+1.0; n++;
        }
    }

    seg_nq = n / 4;
    tg_acct(TG_ACCT_TERRAIN, si);      /* one slab covering both verges */
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, &seg_page, &seg_nq, 1);
}

/* [R7 item 18] "Water near the road but trees floating in the air ... add a
 * check to see if trees are touching ground, and if it's water or a coastline
 * trees should not be placed." Two rules, one gate:
 *
 * 1. GROUND. The tree base used n->y -- the ROAD height -- so a tree set back
 *    onto terrain that falls away from the road hung in the air above it. Sample
 *    the drop-below-road at the trunk's lateral distance from the SAME
 *    cross-section the ground mesh is built from (tg_ground_side), and stand the
 *    tree on that. Away from a coast the skirt is near-flat (70 raw), so this is
 *    a no-op there; on a seaward coast the terrain ramps down and this makes the
 *    trunk follow it instead of floating.
 *
 * 2. WATER / COASTLINE. On a coastal run's SEAWARD side the terrain leaves a flat
 *    verge (TD5_TG_SHORE_VERGE) and then ramps THROUGH sea level to the shore.
 *    Anything planted past that flat verge is on the beach ramp or the sea, so
 *    refuse it -- exactly the span-1120 "trees floating over water" report. The
 *    landward side and non-coastal biomes are unaffected.
 *
 * Bridge decks are already skipped by both callers, so the gorge branch of
 * tg_ground_side never applies here. Gated by TD5RE_R7_FLORA (default ON; =0
 * restores the R6 behaviour of planting at road height with no water check). */
static int tg_flora_plant(const TG_NodeList *nl, int si, const TG_Biome *b,
                          double side, double gap, double tw,
                          double *cx, double *cz, double *base_y)
{
    const TG_Node *n = &nl->v[si];
    const double d  = gap + tw * 0.5;             /* trunk distance from road edge */
    const double lx = n->tz * side, lz = -n->tx * side;
    const double water_side = b->water ? tg_water_side(si) : 0.0;
    TG_GroundProf p;
    double dy;
    int k;

    *cx = n->x + lx * (n->width * 0.5 + gap + tw * 0.5);
    *cz = n->z + lz * (n->width * 0.5 + gap + tw * 0.5);
    *base_y = n->y;

    if (!td5_env_flag_on("TD5RE_R7_FLORA")) return 1;   /* A/B: R6 behaviour */

    /* Rule 2: over the seaward beach/sea of a coastal run -> no tree. */
    if (b->water && side == tg_water_side(si) && d > (double)TD5_TG_SHORE_VERGE)
        return 0;

    /* Rule 1: rest on the sampled terrain, not at road height. */
    tg_ground_side(nl, si, side > 0.0, water_side, &p);
    if (d <= p.d[0]) {
        dy = p.dy[0];
    } else if (d >= p.d[p.n - 1]) {
        dy = p.dy[p.n - 1];
    } else {
        dy = p.dy[p.n - 1];
        for (k = 0; k + 1 < p.n; k++)
            if (d <= p.d[k + 1]) {
                const double t = (d - p.d[k]) / (p.d[k + 1] - p.d[k]);
                dy = p.dy[k] + t * (p.dy[k + 1] - p.dy[k]);
                break;
            }
    }
    *base_y = n->y - dy;
    return 1;
}

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

/* [R7 item 12] The median (gore floor) surface page, held STABLE for the whole
 * length of a fork. tg_emit_gore used to key its page on tg_biome_for_span(si) --
 * the BLENDED biome index -- so where a biome boundary crosses a fork (seed 99991
 * fork 3, 510-631, enters CITY at 600) the median floor DITHERED between the two
 * biomes' ground pages span by span, the "median switches between grass and
 * tiles" report (item 12, span 611). Keyed to the fork's HARD start cell, exactly
 * as the divider TREATMENT keys on the fork ordinal (tg_emit_avenue_divider,
 * fork_index % 3), so ONE material runs the length of the median instead of a
 * per-span roll. */
static int tg_fork_gore_page(int fork_index)
{
    if (fork_index < 0 || fork_index >= s_fork_count)
        return k_biomes[0].ground_page;
    return k_biomes[tg_biome_cell_index(s_forks[fork_index].F)].ground_page;
}

/* `half_n` / `half_f` are the branch carriageway's OWN half width at each end of
 * the span, not a fixed quarter of the road. Until 2026-08-27 this computed
 * width*0.25 internally, which was the half width of a FIXED half carriageway
 * -- once the corridor could widen and taper (tg_branch_wscale) the gore stopped
 * reaching the branch's left edge and re-opened the see-through slit at the
 * mouth that TD5_TG_GORE_OVERLAP exists to close. */
static int tg_emit_gore(const TG_NodeList *nl, int si,
                        double shift_n, double shift_f,
                        double half_n, double half_f, int ground_page,
                        TG_Buf *blk)
{
    const TG_Node *a = &nl->v[si], *c = &nl->v[si + 1];
    const double drop = TD5_TG_GORE_DROP;
    const double ov   = TD5_TG_GORE_OVERLAP;
    /* Branch left edge, pushed a further `ov` to the RIGHT (lateral is +ve to
     * the left of travel, and the branch sits at negative lateral). */
    double tnr = shift_n + half_n - ov;            /* near */
    double tfr = shift_f + half_f - ov;            /* far  */
    double px[4], py[4], pz[4], uu[4], vv[4];
    double cx = 0, cy = 0, cz = 0, radius = 0;
    int i;

    /* near-left = road centre pushed `ov` INTO the main carriageway,
     * near-right = branch left edge pushed `ov` into the branch, then far. */
    px[0]=a->x+a->tz*ov;    py[0]=a->y-drop; pz[0]=a->z-a->tx*ov;    uu[0]=0.0; vv[0]=(double)si;
    px[1]=a->x+a->tz*tnr;   py[1]=a->y-drop; pz[1]=a->z-a->tx*tnr;   uu[1]=1.0; vv[1]=(double)si;
    px[2]=c->x+c->tz*tfr;   py[2]=c->y-drop; pz[2]=c->z-c->tx*tfr;   uu[2]=1.0; vv[2]=(double)si+1.0;
    px[3]=c->x+c->tz*ov;    py[3]=c->y-drop; pz[3]=c->z-c->tx*ov;    uu[3]=0.0; vv[3]=(double)si+1.0;

    for (i = 0; i < 4; i++) { cx += px[i]; cy += py[i]; cz += pz[i]; }
    cx /= 4; cy /= 4; cz /= 4;
    for (i = 0; i < 4; i++) {
        double dx=px[i]-cx, dy=py[i]-cy, dz=pz[i]-cz;
        double d = sqrt(dx*dx+dy*dy+dz*dz);
        if (d > radius) radius = d;
    }
    if (!(radius > 0.0)) radius = 1.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 0);
    tg_put_u32(blk, 1);
    tg_put_u32(blk, 4);
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx); tg_put_f32(blk, cy); tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);
    tg_put_u16(blk, (unsigned)ground_page);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);
    tg_put_u16(blk, 1);                    /* one quad */
    tg_put_u32(blk, 0);
    for (i = 0; i < 4; i++) {
        tg_put_f32(blk, px[i]); tg_put_f32(blk, py[i]); tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);
        tg_put_f32(blk, uu[i]); tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    return !blk->oom;
}

/* ===================== BRANCH-CORRIDOR PAVEMENT (item 9a) =====================
 * The branch carriageway of a fork is APPENDED after the ring and never sees the
 * city hooks (tg_emit_fb_city runs on main-ring spans only), so a corridor was
 * bare road meeting open ground -- the "missing gaps between road and sidewalk
 * on branches" report. This lays a pavement along the branch's OUTER edge, on
 * the shipped sidewalk page, derived from the SAME shift/width the corridor road
 * mesh used (tg_branch_shift_s / tg_branch_wscale with the fork's own sep) so it
 * follows the bow and the taper instead of a fixed quarter-road assumption.
 *
 * Coordinate frame is identical to tg_append_row and tg_emit_gore: a point at
 * lateral t (POSITIVE = left of travel) off node n is
 * (n->x + n->tz*t, n->y, n->z - n->tx*t). The branch sits at NEGATIVE lateral;
 * its outer edge is the most-negative point, so the pavement runs further
 * negative still (t decreasing).
 *
 * `mb` is the base MAIN node the corridor step rides on; `acct_si` is the
 * APPENDED corridor span, passed only so the element inventory brackets the
 * pavement at the corridor spans (>= ring) -- that is what proves, without a
 * frame, that scenery now reaches the branch. */
static int tg_emit_branch_sidewalk(const TG_NodeList *nl, int mb, int k, int L,
                                    double sep, int br_lanes, const TG_Biome *b,
                                    TG_Buf *blk, size_t *moff, int *nmesh,
                                    int acct_si)
{
    const TG_Node *a = &nl->v[mb];
    const TG_Node *c = &nl->v[mb + 1];
    const double sw = tg_city_sidewalk_w(b);
    const double kh = tg_city_kerb_h(b);
    /* Branch centre and half width at each end, from the shared helpers. */
    const double sh0 = tg_branch_shift_s(k,     L, a->width, sep);
    const double sh1 = tg_branch_shift_s(k + 1, L, c->width, sep);
    const double h0  = a->width * tg_branch_wscale_s(k,     L, br_lanes, sep) * 0.5;
    const double h1  = c->width * tg_branch_wscale_s(k + 1, L, br_lanes, sep) * 0.5;
    const double e0  = sh0 - h0;                 /* outer (right) edge, near */
    const double e1  = sh1 - h1;                 /* outer (right) edge, far  */
    const double u_w = sw / (double)TD5_TG_SPAN_LENGTH;
    const double u_k = kh / (double)TD5_TG_SPAN_LENGTH;
    double px[8], py[8], pz[8], uu[8], vv[8];
    int seg_page = TD5_TG_PAGE_SIDEWALK, seg_nq;
    int n = 0;

    if (sw <= 0.0) return 1;

    /* Top slab: near-edge, near-outer, far-outer, far-edge -- the up-facing
     * winding tg_emit_gore uses (near-high-t, near-low-t, far-low-t, far-high-t;
     * the outer point is at LOWER t because the branch is at negative lateral). */
    px[n]=a->x+a->tz*e0;        py[n]=a->y+kh; pz[n]=a->z-a->tx*e0;        uu[n]=0.0;  vv[n]=0.0; n++;
    px[n]=a->x+a->tz*(e0-sw);   py[n]=a->y+kh; pz[n]=a->z-a->tx*(e0-sw);   uu[n]=u_w;  vv[n]=0.0; n++;
    px[n]=c->x+c->tz*(e1-sw);   py[n]=c->y+kh; pz[n]=c->z-c->tx*(e1-sw);   uu[n]=u_w;  vv[n]=1.0; n++;
    px[n]=c->x+c->tz*e1;        py[n]=c->y+kh; pz[n]=c->z-c->tx*e1;        uu[n]=0.0;  vv[n]=1.0; n++;

    /* Kerb face, facing the branch carriageway (toward +lateral): base on the
     * asphalt, top at the slab. */
    px[n]=a->x+a->tz*e0; py[n]=a->y;    pz[n]=a->z-a->tx*e0; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*e0; py[n]=a->y+kh; pz[n]=a->z-a->tx*e0; uu[n]=u_k; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*e1; py[n]=c->y+kh; pz[n]=c->z-c->tx*e1; uu[n]=u_k; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*e1; py[n]=c->y;    pz[n]=c->z-c->tx*e1; uu[n]=0.0; vv[n]=1.0; n++;

    seg_nq = n / 4;
    tg_acct_n(TG_ACCT_SIDEWALK, acct_si, 1);
    moff[(*nmesh)++] = blk->len;
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, &seg_page, &seg_nq, 1);
}

/* [R5 STRUCT item 10] Flat VERGE BAND along a branch corridor's OUTER edge.
 *
 * tg_emit_branch_sidewalk above lays a raised kerb+slab, but ONLY where the
 * corridor's base main node is a PAVED (city) biome -- tg_city_sidewalk_w is 0
 * for the billboard/tree biomes. On seed 99991 fork 2 (F=510) runs its corridor
 * mostly through ORIENTAL (main nodes 511-599), so its whole start had NO edge
 * treatment at all: bare road meeting open ground on the right, the reported
 * "no sidewalk on the right side of the branch". Meanwhile the ORIENTAL MAIN
 * road gets the flat verge band (tg_city_emit_verge_band). This gives the branch
 * the SAME out-of-town margin on its outer edge, mirroring the main road, so the
 * corridor is no longer bare where its biome has no pavement. Flat (no kerb),
 * one quad, lifted TD5_TG_VERGE_LIFT off the ground like the main-road band, on
 * the shipped sidewalk page. Frame and outer-edge derivation identical to
 * tg_emit_branch_sidewalk. Default ON; TD5RE_R5_BRANCH_VERGE=0 for an A/B. */
static int tg_emit_branch_verge(const TG_NodeList *nl, int mb, int k, int L,
                                double sep, int br_lanes, double bw,
                                TG_Buf *blk, size_t *moff, int *nmesh,
                                int acct_si)
{
    const TG_Node *a = &nl->v[mb];
    const TG_Node *c = &nl->v[mb + 1];
    const double sh0 = tg_branch_shift_s(k,     L, a->width, sep);
    const double sh1 = tg_branch_shift_s(k + 1, L, c->width, sep);
    const double h0  = a->width * tg_branch_wscale_s(k,     L, br_lanes, sep) * 0.5;
    const double h1  = c->width * tg_branch_wscale_s(k + 1, L, br_lanes, sep) * 0.5;
    const double e0  = sh0 - h0;                 /* outer (right) edge, near */
    const double e1  = sh1 - h1;                 /* outer (right) edge, far  */
    const double lift = TD5_TG_VERGE_LIFT;
    const double u_w = bw / (double)TD5_TG_SPAN_LENGTH;
    double px[4], py[4], pz[4], uu[4], vv[4];
    int seg_page = TD5_TG_PAGE_SIDEWALK, seg_nq;
    int n = 0;

    if (bw <= 0.0) return 1;
    /* [R6 items 9 & 5] The INNER edge sits at ROAD LEVEL (a->y / c->y), the outer
     * edge at +lift. The band used to sit ENTIRELY at +lift, which floated its
     * whole inner edge TD5_TG_VERGE_LIFT above the carriageway it abuts. On the
     * main road the ground skirt fills behind that step, but a branch corridor
     * has NO skirt (pad + corridor carry road only), so the step was open to the
     * void -- the "small see-through gap between the branch and the sidewalk"
     * (item 9), and the floating band parallaxing against the road as the car
     * moved read as "the floor on the right side of the branch MOVES" (item 5).
     * Meeting the road edge exactly and ramping up to the lift outboard closes
     * both. Winding unchanged: near-edge, near-outer, far-outer, far-edge; the
     * branch is at negative lateral so "further out" is t decreasing (e - bw). */
    px[n]=a->x+a->tz*e0;        py[n]=a->y;      pz[n]=a->z-a->tx*e0;        uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*(e0-bw);   py[n]=a->y+lift; pz[n]=a->z-a->tx*(e0-bw);   uu[n]=u_w; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*(e1-bw);   py[n]=c->y+lift; pz[n]=c->z-c->tx*(e1-bw);   uu[n]=u_w; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*e1;        py[n]=c->y;      pz[n]=c->z-c->tx*e1;        uu[n]=0.0; vv[n]=1.0; n++;

    seg_nq = n / 4;
    tg_acct_n(TG_ACCT_BRANCH_VERGE, acct_si, 1);
    moff[(*nmesh)++] = blk->len;
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, &seg_page, &seg_nq, 1);
}

/* [R7 item 10] SCENERY on the branch corridor's GRASS verge. tg_emit_branch_verge
 * lays a flat grass band along the corridor's outer edge, but nothing DRESSES it,
 * so a fork through a tree biome (seed 99991 fork 2 runs its corridor through
 * ORIENTAL) showed bare grass and nothing else beside the branch -- "barely any
 * grass to the side and nothing else ... add more scenery on the side if it is
 * grass" (item 10, span 569). This plants the biome's OWN roadside tree billboards
 * along that verge, mirroring the main road's roadside trees (tg_building_for_span
 * tree path), so the branch flank reads like the main road's instead of a bald
 * strip.
 *
 * PLACEMENT clears the branch through the shared carriageway authority: the
 * setback is measured from the MAIN road edge and pushed out past the bowed
 * corridor's CURRENT width by tg_carriageway_clear_gap on the RIGHT side, so a
 * trunk can never land on the branch carriageway (the GUARD contract -- the same
 * adoption facades, terrain skirts and the fork backdrop use). Billboard (grass)
 * biomes only; where the biome has no trees nothing is added and the verge is
 * unchanged. `mb` is the base MAIN node the corridor step rides on; `acct_si` is
 * the appended corridor span, so the inventory brackets the tree at the corridor
 * spans (>= ring) -- what proves, without a frame, that scenery now reaches the
 * branch flank. Default ON; TD5RE_R7_BRANCH_FLORA=0 for an A/B. */
static int tg_emit_branch_flora(const TG_NodeList *nl, int mb,
                                const TG_Biome *b, TG_Buf *blk,
                                size_t *moff, int *nmesh, int acct_si)
{
    const TG_Node *n = &nl->v[mb];
    unsigned int h = (unsigned)mb * 2654435761u;
    const TG_TreePage *tp;
    double jit, tw, th, gap, set, cx, cz;
    int tv;

    if (!b->billboard || b->tree_n <= 0) return 1;
    if (!td5_env_flag_on("TD5RE_R7_BRANCH_FLORA")) return 1;
    /* One tree in roughly half of the corridor spans: a dressed verge, not a
     * solid wall of trunks hugging the branch. */
    if ((h >> 28) & 1u) return 1;

    tv  = b->tree_set[(h >> 13) % (unsigned)b->tree_n];
    tp  = &k_tree_pages[tv];
    jit = 0.8 + (double)((h >> 9) % 41) * 0.01;        /* 0.80 .. 1.20 */
    tw  = (double)tp->w * jit;
    th  = (double)tp->h * jit;
    /* Base gap past the branch's own grass verge, then clear_gap lifts it past
     * the bowed corridor so the trunk clears the branch at its ACTUAL width. */
    gap = tg_verge_band_w(b) + 500.0 + (double)((h >> 5) % 1800);
    set = n->width * 0.5
        + tg_carriageway_clear_gap(nl, mb, -1.0, gap, TD5_TG_CARRIAGEWAY_MARGIN)
        + tw * 0.5;
    /* Right of travel is NEGATIVE lateral: point at lateral t off node n is
     * (n->x + n->tz*t, n->y, n->z - n->tx*t). */
    cx = n->x + n->tz * (-set);
    cz = n->z - n->tx * (-set);
    tg_acct_n(TG_ACCT_R7_BRANCH, acct_si, 1);
    moff[(*nmesh)++] = blk->len;
    return tg_emit_billboard_mesh(blk, cx, n->y, cz, tw * 0.5, th,
                                  tg_tree_slot(tv), 1);
}

/* ===================== AVENUE DIVIDER (items 9c / 10) =====================
 * Where a fork is TIGHT -- a divided AVENUE rather than a road splitting in two
 * (tg_fork_is_avenue) -- the gore between the two carriageways is a slim central
 * strip, and a real avenue puts something ON it. This raises a median island
 * down the centre of the gore. THREE treatments, chosen by the fork's ordinal so
 * a track with several avenues shows all of them: a planted strip, a concrete
 * barrier, and a plain kerbed island ("a distinction of different dividers").
 *
 * A 3-quad prism (top + the two road-facing side walls), not a single decal,
 * because the car passes on BOTH sides and each face must be visible from its
 * carriageway even with backface culling on -- the same reasoning the guardrail
 * prism documents. Windings verified against the gore's up-facing top quad.
 *
 * The island pinches to nothing at the fork and the rejoin (the gore does too),
 * so it opens out of and closes back into the full-width road like a real
 * avenue median opening from an intersection. `sh*`/`half*` are the branch shift
 * and half width at the span's two ends, exactly as passed to tg_emit_gore. */
static int tg_emit_avenue_divider(const TG_NodeList *nl, int si, int fork_index,
                                  double sh0, double sh1, double half0,
                                  double half1, TG_Buf *blk, size_t *moff,
                                  int *nmesh)
{
    const TG_Node *a = &nl->v[si];
    const TG_Node *c = &nl->v[si + 1];
    /* The gore runs from the main road's right edge (lateral ~0) to the branch's
     * LEFT edge (sh+half, negative). Median centre = halfway; gore width = how
     * far that edge is from the road centre. */
    const double bl0 = sh0 + half0, bl1 = sh1 + half1;   /* branch left edges */
    const double gw0 = -bl0, gw1 = -bl1;                 /* gore widths (>=0)  */
    const int    treat = ((unsigned)fork_index) % 3;     /* 0 plant 1 bar 2 kerb */
    const double mw_cap = (treat == 1) ? 260.0 : 520.0;  /* island half width  */
    const double H      = (treat == 1) ? 360.0 : (treat == 0 ? 220.0 : 150.0);
    const int    page   = (treat == 0) ? TD5_TG_PAGE_GREEN
                        : (treat == 1) ? TD5_TG_PAGE_RAIL : TD5_TG_PAGE_SIDEWALK;
    /* Item 8: the two vertical side walls must NOT wear the TOP page. A planted
     * median (treat 0, GREEN top) painted grass up its walls -- verbatim "grass
     * as walls is wrong"; a kerbed island (treat 2) put paving slabs on its
     * walls. Both want a concrete KERB face on the sides. A concrete barrier
     * (treat 1, RAIL) is a wall material through and through, so it keeps RAIL on
     * every face. Per-quad paging via a 2-segment mesh: seg 0 = the single TOP
     * quad on `page`, seg 1 = the two side-wall quads on `side_page`. */
    const int    side_page = (treat == 1) ? TD5_TG_PAGE_RAIL : TD5_TG_PAGE_BRANCH_KERB;
    double mc0, mc1, mw0, mw1, cl0, cr0, cl1, cr1, base0, base1;
    double px[12], py[12], pz[12], uu[12], vv[12];
    int seg_page[2], seg_nq[2];
    int n = 0;

    /* No island where the gore is a mere sliver (near the mouths): a 100-unit
     * strip of raised concrete popping in and out reads worse than nothing. */
    if (gw0 < 200.0 && gw1 < 200.0) return 1;

    mc0 = bl0 * 0.5; mc1 = bl1 * 0.5;                    /* median centre lateral */
    mw0 = gw0 * 0.32; if (mw0 > mw_cap) mw0 = mw_cap; if (mw0 < 0.0) mw0 = 0.0;
    mw1 = gw1 * 0.32; if (mw1 > mw_cap) mw1 = mw_cap; if (mw1 < 0.0) mw1 = 0.0;
    cl0 = mc0 + mw0; cr0 = mc0 - mw0;                    /* road side / branch side */
    cl1 = mc1 + mw1; cr1 = mc1 - mw1;
    /* Sit the base at the gore's own level (4 below road) so the island rises
     * out of the median rather than floating a hair above it. */
    base0 = a->y - TD5_TG_GORE_DROP; base1 = c->y - TD5_TG_GORE_DROP;

    /* TOP (up-facing): near-road, near-branch, far-branch, far-road. */
    px[n]=a->x+a->tz*cl0; py[n]=base0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*cr0; py[n]=base0+H; pz[n]=a->z-a->tx*cr0; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=base1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=base1+H; pz[n]=c->z-c->tx*cl1; uu[n]=0.0; vv[n]=1.0; n++;

    /* ROAD-side wall (faces +lateral, toward the main carriageway). */
    px[n]=a->x+a->tz*cl0; py[n]=base0;   pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=1.0; n++;
    px[n]=a->x+a->tz*cl0; py[n]=base0+H; pz[n]=a->z-a->tx*cl0; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=base1+H; pz[n]=c->z-c->tx*cl1; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=c->x+c->tz*cl1; py[n]=base1;   pz[n]=c->z-c->tx*cl1; uu[n]=1.0; vv[n]=1.0; n++;

    /* BRANCH-side wall (faces -lateral, toward the corridor). */
    px[n]=a->x+a->tz*cr0; py[n]=base0;   pz[n]=a->z-a->tx*cr0; uu[n]=0.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=base1;   pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=c->x+c->tz*cr1; py[n]=base1+H; pz[n]=c->z-c->tx*cr1; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=a->x+a->tz*cr0; py[n]=base0+H; pz[n]=a->z-a->tx*cr0; uu[n]=0.0; vv[n]=0.0; n++;

    /* TOP quad (verts 0..3) on `page`; the two side walls (verts 4..11) on
     * `side_page`. Segments consume quads sequentially in vertex order, so this
     * split matches the emit order above exactly. */
    seg_page[0] = page;      seg_nq[0] = 1;
    seg_page[1] = side_page; seg_nq[1] = 2;
    /* Accounted as a FENCE (a linear median structure) rather than a new
     * inventory kind, to keep the shared enum untouched for the parallel batch. */
    tg_acct_n(TG_ACCT_FENCE, si, 1);
    moff[(*nmesh)++] = blk->len;
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, seg_page, seg_nq, 2);
}

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

static int tg_guardrails_enabled(void)
{
    /* Default OFF until a frame confirms it, same discipline as tunnels and
     * branches. NOTE td5_env_flag_off() returns 1 only when the value is
     * literally "1" -- it means "opt in", despite the name. */
    return td5_env_flag_off("TD5RE_AUTOTRACK_GUARDRAILS");
}

/* Should span si carry a barrier? Not every span: real roads are lined on the
 * outside of bends and where the road is elevated, not down every straight, and
 * railing all 1800 spans would both look wrong and cost 1800 extra meshes. */
static int tg_span_needs_guardrail_raw(const TG_NodeList *nl, int si, int nspans)
{
    /* 50 = 5 deg of heading change across the span. MEASURED over 3 seeds this
     * rails 11-15% of spans, consistently -- barriers through the corners and
     * not down the straights, which is the point. The previous default of 15
     * (1.5 deg) railed 62%, i.e. nearly everything. Curvature is the stable
     * signal here: unlike the elevation gate its coverage barely moves between
     * seeds. */
    const int limit = td5_env_int("TD5RE_AUTOTRACK_RAIL_DEG10", 50, 0, 900);
    double cross, dot, ang_deg;

    if (si < 1 || si + 2 >= nl->count) return 0;

    /* EXCLUSION: inside a tunnel the walls already contain and read as a
     * boundary; a barrier there is invisible clutter. */
    if (tg_span_in_tunnel(si)) return 0;

    /* Forks used to be excluded outright here (a rail derived from the MAIN
     * road edge lands ON the branch carriageway, walling the corridor off), at
     * the cost of a rail-free hole around every fork. tg_emit_guardrail now
     * slides the right-hand barrier out to tg_carriageway_reach instead, so the
     * fork region rails like any other span and the exclusion is gone. */
    (void)nspans;

    /* On a deliberately-placed deck: rail the WHOLE run, including the shallow
     * ends the lift test would miss. This is the case the guardrail work
     * existed for -- nothing stops you leaving a deck sideways. */
    if (tg_span_in_bridge_run(si)) return 1;

    /* Elevated: nothing stops you leaving a bridge deck, so rail it. Judged
     * against the LOCAL terrain line, so this means "on a deck" and not
     * merely "uphill of the lowest point on the track". */
    if (nl->v[si].y - tg_local_ground_y(nl, si) >= TD5_TG_BRIDGE_MIN_LIFT)
        return 1;

    /* Bend: heading change across this span, in tenths of a degree. */
    cross = nl->v[si].tx * nl->v[si + 1].tz - nl->v[si].tz * nl->v[si + 1].tx;
    dot   = nl->v[si].tx * nl->v[si + 1].tx + nl->v[si].tz * nl->v[si + 1].tz;
    ang_deg = atan2(fabs(cross), dot) * 180.0 / TD5_TG_PI;
    return (ang_deg * 10.0) >= (double)limit;
}

/* RUN DILATION. The raw test above is per-span, and its ELEVATION half is
 * judged against a +/-8 span average (tg_local_ground_y), so on undulating or
 * graded ground it latches for one or two spans at a time -- a crest trips it,
 * the approach and the exit do not. The result is ONE-SPAN RAIL ISLANDS: a
 * 1500-unit stub of barrier that begins and ends in mid-air on a slope, which is
 * the other half of the "fences on slope" report (a rail that stops on a grade
 * reads as stair-stepping, and one that starts mid-grade reads as floating).
 *
 * Fix: rail a span if ANY span within +/-TD5RE_AUTOTRACK_RAIL_PAD needs one.
 * That bridges the gaps between islands and extends every run past its ends, so
 * a barrier always starts and finishes on road the raw test agreed was worth
 * railing. Pure function of nl -- no state, so the streaming range emitter can
 * still ask about any span in isolation. */
static int tg_span_needs_guardrail(const TG_NodeList *nl, int si, int nspans)
{
    const int pad = td5_env_int("TD5RE_AUTOTRACK_RAIL_PAD", 3, 0, 16);
    int k;

    /* Exclusions must be re-checked on THIS span, not on the neighbour that
     * satisfied the test: dilating a rail into a tunnel is exactly what that
     * exclusion exists to prevent. */
    if (tg_span_in_tunnel(si)) return 0;

    for (k = -pad; k <= pad; k++)
        if (tg_span_needs_guardrail_raw(nl, si + k, nspans)) return 1;
    return 0;
}

/* One barrier prism per side for span si. Caller gates with
 * tg_span_needs_guardrail.
 *
 * PITCH (2026-08-26, "fences on slope should be inclined to follow the slope").
 * The rail used to be extruded straight UP: top = edge_y + RAIL_HEIGHT, base =
 * edge_y - BASE_DROP, both purely vertical. On a graded span that leaves the
 * barrier standing plumb while the road it guards is inclined, so its inner face
 * is not perpendicular to the tarmac and its top edge is not parallel to the
 * road -- and where two spans of different grade meet, the two plumb rails join
 * at a visible kink instead of a continuous line. Real crash barriers lean with
 * the road.
 *
 * So the extrusion axis is now the road's SURFACE NORMAL in the pitch plane:
 * with grade g = dY/d(along-road), the along-road unit is (1, g)/|.| and the
 * normal is (-g, 1)/|.|, both in the (along-road, up) plane. Offsetting by
 * height*normal instead of height*up tilts the posts back by exactly the road's
 * pitch, which is what "inclined to follow the slope" means. At g = 0 the normal
 * IS up, so a flat span is bit-identical to the old behaviour and only graded
 * spans move. The lateral offsets are untouched -- camber is not modelled here,
 * and the two edges already carry their own heights.
 */
static int tg_emit_guardrail(const TG_NodeList *nl, int si, TG_Buf *blk)
{
    /* Road grade over this span, and the pitch-plane normal derived from it.
     * ny is the vertical part of the normal and nt the along-road part, so a
     * point offset by h is (p + nt*h*along, p_y + ny*h). */
    double grade = 0.0, gm, n_up, n_along;
    double atx = 0.0, atz = 0.0;      /* along-road unit, horizontal part */
    double nlx, nly, nlz, nrx, nry, nrz;   /* near left / right road edge */
    double flx, fly, flz, frx, fry, frz;   /* far  left / right road edge */
    double nux, nuz, fux, fuz;             /* outward lateral units */
    double len, cx, cy, cz, radius = 0.0;
    double px[24], py[24], pz[24], uu[24], vv[24];
    int side, i, n = 0;

    tg_road_edge(nl, si, 0.0, 0.0, 1.0, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
    tg_road_edge(nl, si, 1.0, 0.0, 1.0, &flx, &fly, &flz, &frx, &fry, &frz);

    /* Grade of THIS span, from the centerline nodes rather than the edges, so a
     * width change across the span cannot be mistaken for pitch. */
    {
        double dx = nl->v[si + 1].x - nl->v[si].x;
        double dz = nl->v[si + 1].z - nl->v[si].z;
        double dh = sqrt(dx * dx + dz * dz);
        if (dh > 1e-6) {
            atx = dx / dh; atz = dz / dh;
            grade = (nl->v[si + 1].y - nl->v[si].y) / dh;
        }
    }
    gm      = sqrt(1.0 + grade * grade);
    n_up    =  1.0 / gm;            /* vertical part of the surface normal   */
    n_along = -grade / gm;          /* along-road part (leans against the climb) */

    nux = nlx - nrx; nuz = nlz - nrz;
    len = sqrt(nux * nux + nuz * nuz);
    if (len < 1e-6) { nux = 1.0; nuz = 0.0; } else { nux /= len; nuz /= len; }
    fux = flx - frx; fuz = flz - frz;
    len = sqrt(fux * fux + fuz * fuz);
    if (len < 1e-6) { fux = 1.0; fuz = 0.0; } else { fux /= len; fuz /= len; }

    for (side = 0; side < 2; side++) {
        /* side 0 = left edge (outward = +unit), side 1 = right (outward = -). */
        const double s  = side ? -1.0 : 1.0;
        const double ex = side ? nrx : nlx, ey = side ? nry : nly;
        const double ez = side ? nrz : nlz;
        const double gx = side ? frx : flx, gy = side ? fry : fly;
        const double gz = side ? frz : flz;
        /* Outboard push, from the ONE carriageway authority. Over a fork the
         * branch has bowed out beyond the main road edge these points come
         * from, so a barrier at the bare edge would stand in the corridor;
         * asking for a zero setback returns exactly how far out the outermost
         * tarmac is. Zero on the left (nothing bows that way) and zero on every
         * span off a fork, so ordinary road is bit-identical. Near and far are
         * asked separately so the rail follows the bow instead of stepping. */
        const double push_n = side
            ? tg_carriageway_clear_gap(nl, si, -1.0, 0.0, 0.0) : 0.0;
        const double push_f = side
            ? tg_carriageway_clear_gap(nl, si + 1, -1.0, 0.0, 0.0) : 0.0;
        const double o0 = TD5_TG_RAIL_OFFSET;
        const double o1 = TD5_TG_RAIL_OFFSET + TD5_TG_RAIL_THICK;

        /* SAFEGUARD: never leave a barrier standing on tarmac. Asked of the
         * same authority the push came from, so in a consistent world it cannot
         * fire (the rail line is reach + RAIL_OFFSET, one offset OUTSIDE the
         * reach). It exists for the inconsistent world: if some future
         * carriageway grows past its own reported reach, dropping the rail is
         * the right failure -- a missing barrier is a cosmetic loss, a barrier
         * across a live lane is a wall. The LEFT side can never trip it (no
         * corridor bows left, so left reach IS the road edge), which is what
         * keeps the mesh non-empty and the quad count below >= 3. */
        if (tg_on_carriageway(nl, si, s * (tg_road_half_width(nl, si)
                                           + o0 + push_n), 0.0) ||
            tg_on_carriageway(nl, si + 1, s * (tg_road_half_width(nl, si + 1)
                                               + o0 + push_f), 0.0))
            continue;
        /* Along-road slide that goes with a given height offset, so the post
         * leans with the road's pitch instead of standing plumb. Zero on a flat
         * span (n_along = 0), which keeps flat road byte-identical. */
        const double slide_t = n_along * TD5_TG_RAIL_HEIGHT;
        const double slide_b = n_along * -TD5_TG_RAIL_BASE_DROP;
        /* near/far x inner/outer, at base and top -- the lateral offsets are the
         * same as before, the pitch lean is the extra atx/atz term on the TOP
         * and BASE rows. */
        const double nib_x = ex + s * nux * (o0 + push_n);
        const double nib_z = ez + s * nuz * (o0 + push_n);
        const double nob_x = ex + s * nux * (o1 + push_n);
        const double nob_z = ez + s * nuz * (o1 + push_n);
        const double fib_x = gx + s * fux * (o0 + push_f);
        const double fib_z = gz + s * fuz * (o0 + push_f);
        const double fob_x = gx + s * fux * (o1 + push_f);
        const double fob_z = gz + s * fuz * (o1 + push_f);
        /* Top and base rows, offset along the SURFACE NORMAL. */
        const double nibt_x = nib_x + atx * slide_t, nibt_z = nib_z + atz * slide_t;
        const double nobt_x = nob_x + atx * slide_t, nobt_z = nob_z + atz * slide_t;
        const double fibt_x = fib_x + atx * slide_t, fibt_z = fib_z + atz * slide_t;
        const double fobt_x = fob_x + atx * slide_t, fobt_z = fob_z + atz * slide_t;
        const double nibb_x = nib_x + atx * slide_b, nibb_z = nib_z + atz * slide_b;
        const double nobb_x = nob_x + atx * slide_b, nobb_z = nob_z + atz * slide_b;
        const double fibb_x = fib_x + atx * slide_b, fibb_z = fib_z + atz * slide_b;
        const double fobb_x = fob_x + atx * slide_b, fobb_z = fob_z + atz * slide_b;
        const double nyb = ey - n_up * TD5_TG_RAIL_BASE_DROP;
        const double fyb = gy - n_up * TD5_TG_RAIL_BASE_DROP;
        const double nyt = ey + n_up * TD5_TG_RAIL_HEIGHT;
        const double fyt = gy + n_up * TD5_TG_RAIL_HEIGHT;
        const double u0 = (double)si, u1 = (double)si + 1.0;

        /* INNER face (towards the road). U runs along the road, V up the face,
         * so the page's bottom rows land at the base -- see the page comment. */
        px[n]=nibb_x; py[n]=nyb; pz[n]=nibb_z; uu[n]=u0; vv[n]=0.0; n++;
        px[n]=nibt_x; py[n]=nyt; pz[n]=nibt_z; uu[n]=u0; vv[n]=1.0; n++;
        px[n]=fibt_x; py[n]=fyt; pz[n]=fibt_z; uu[n]=u1; vv[n]=1.0; n++;
        px[n]=fibb_x; py[n]=fyb; pz[n]=fibb_z; uu[n]=u1; vv[n]=0.0; n++;

        /* OUTER face, wound the other way so it faces away from the road. */
        px[n]=fobb_x; py[n]=fyb; pz[n]=fobb_z; uu[n]=u1; vv[n]=0.0; n++;
        px[n]=fobt_x; py[n]=fyt; pz[n]=fobt_z; uu[n]=u1; vv[n]=1.0; n++;
        px[n]=nobt_x; py[n]=nyt; pz[n]=nobt_z; uu[n]=u0; vv[n]=1.0; n++;
        px[n]=nobb_x; py[n]=nyb; pz[n]=nobb_z; uu[n]=u0; vv[n]=0.0; n++;

        /* TOP cap, so the barrier reads as solid from a chase camera. */
        px[n]=nibt_x; py[n]=nyt; pz[n]=nibt_z; uu[n]=u0; vv[n]=1.0; n++;
        px[n]=nobt_x; py[n]=nyt; pz[n]=nobt_z; uu[n]=u0; vv[n]=0.85; n++;
        px[n]=fobt_x; py[n]=fyt; pz[n]=fobt_z; uu[n]=u1; vv[n]=0.85; n++;
        px[n]=fibt_x; py[n]=fyt; pz[n]=fibt_z; uu[n]=u1; vv[n]=1.0; n++;
    }

    cx = cy = cz = 0.0;
    for (i = 0; i < n; i++) { cx += px[i]; cy += py[i]; cz += pz[i]; }
    cx /= n; cy /= n; cz /= n;
    for (i = 0; i < n; i++) {
        double dx = px[i]-cx, dy = py[i]-cy, dz = pz[i]-cz;
        double d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d > radius) radius = d;
    }
    if (!(radius > 0.0)) radius = 1.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 0);                    /* opaque, not a billboard */
    tg_put_u32(blk, 1);
    tg_put_u32(blk, (unsigned)n);
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx); tg_put_f32(blk, cy); tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    tg_put_u16(blk, 0);                    /* dispatch_type 0 */
    tg_put_u16(blk, TD5_TG_PAGE_RAIL);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);                    /* triangle_count */
    /* 3 quads per side, but the safeguard above may have dropped the right one,
     * so DERIVE the count from the vertices actually written rather than
     * hardcoding 6 -- a quad count that overruns the vertex array is read as
     * garbage geometry, not as a missing rail. */
    tg_put_u16(blk, (unsigned)(n / 4));
    tg_put_u32(blk, 0);

    for (i = 0; i < n; i++) {
        tg_put_f32(blk, px[i]); tg_put_f32(blk, py[i]); tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);
        tg_put_f32(blk, uu[i]); tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    /* One mesh, but one rail per side and the right side can be skipped over a
     * fork, so count RAILS: each pushes 3 quads = 12 verts. */
    tg_acct_n(TG_ACCT_GUARDRAIL, si, n / 12);
    return !blk->oom;
}

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

/* Does span si carry city street furniture at all? Tree biomes have no
 * pavement (tg_city_sidewalk_w returns 0), and a bridge deck carries only its
 * own rails -- a pavement there would hang off the side of the deck. */
static int tg_city_span_paved(const TG_FBHook *h)
{
    if (tg_span_in_bridge_run(h->si)) return 0;
    /* [R7 item 7] hardened city edge -- the raised pavement + kerb HEIGHT run to
     * the true end of the city cell instead of dithering out over the blend band.
     * Keep sw (below) sourced the same way so paved and width never disagree. */
    return tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(h->si)]) > 0.0;
}

/* Lateral frame for span si: both road edges at f=0 and f=1 plus the outward
 * unit at each end. Same computation tg_emit_ground uses, so the pavement
 * follows width changes, curvature and elevation exactly and abuts the skirt
 * without a seam. `out[]` = near-in-x, y, z, far-in-x, y, z, near-ux, near-uz,
 * far-ux, far-uz for side `sg` (+1 = left of travel). */
static void tg_city_edge_frame(const TG_NodeList *nl, int si, double sg,
                               double *out)
{
    double nlx, nly, nlz, nrx, nry, nrz;
    double flx, fly, flz, frx, fry, frz;
    double nux, nuz, fux, fuz, len;

    tg_road_edge(nl, si, 0.0, 0.0, 1.0, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
    tg_road_edge(nl, si, 1.0, 0.0, 1.0, &flx, &fly, &flz, &frx, &fry, &frz);
    nux = nlx - nrx; nuz = nlz - nrz;
    len = sqrt(nux * nux + nuz * nuz);
    if (len < 1e-6) { nux = 1.0; nuz = 0.0; } else { nux /= len; nuz /= len; }
    fux = flx - frx; fuz = flz - frz;
    len = sqrt(fux * fux + fuz * fuz);
    if (len < 1e-6) { fux = 1.0; fuz = 0.0; } else { fux /= len; fuz /= len; }

    if (sg > 0.0) {
        out[0] = nlx; out[1] = nly; out[2] = nlz;
        out[3] = flx; out[4] = fly; out[5] = flz;
        out[6] = nux; out[7] = nuz; out[8] = fux; out[9] = fuz;
    } else {
        out[0] = nrx; out[1] = nry; out[2] = nrz;
        out[3] = frx; out[4] = fry; out[5] = frz;
        out[6] = -nux; out[7] = -nuz; out[8] = -fux; out[9] = -fuz;
    }
}

/* Append one quad (loop order given by the caller) to the vertex arrays. */
static void tg_city_push_quad(double *px, double *py, double *pz,
                              double *uu, double *vv, int *pn,
                              const double *xyz, const double *uv)
{
    int i, n = *pn;
    for (i = 0; i < 4; i++) {
        px[n] = xyz[i * 3 + 0];
        py[n] = xyz[i * 3 + 1];
        pz[n] = xyz[i * 3 + 2];
        uu[n] = uv[i * 2 + 0];
        vv[n] = uv[i * 2 + 1];
        n++;
    }
    *pn = n;
}

/* RAISED PAVEMENT between kerb and facade, one mesh for both sides: a top slab
 * plus the kerb face that closes the step down to the asphalt. The slab top
 * sits at TD5_TG_KERB_H, which is exactly where tg_side_geom now starts the
 * wall, so the two meet flush.
 *
 * UVs are isotropic (u = width/SPAN_LENGTH, v advances one tile per span) for
 * the same reason tg_emit_ground does it: a stretched u both smears the paving
 * and defeats the box-filter mips, which is what made the old ground shimmer. */
static int tg_city_emit_sidewalk(const TG_FBHook *h, double sw)
{
    double px[16], py[16], pz[16], uu[16], vv[16];
    double e[10], q[12], t[8];
    int seg_page = TD5_TG_PAGE_SIDEWALK, seg_nq;
    int s, n = 0;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        const double u_w = sw / (double)TD5_TG_SPAN_LENGTH;
        const double u_k = TD5_TG_KERB_H / (double)TD5_TG_SPAN_LENGTH;
        if (tg_pavement_side_blocked(h->nl, h->si, sg)) continue;
        /* [R6 CROSS item 1] The raised pavement STOPS at a road intersection.
         * Where this side opens onto a side-street mouth (the frontage is a gap
         * here), the slab used to run straight across the mouth at kerb height,
         * so the pavement was "lifted during the crossing" and floated over the
         * cross-street asphalt that tg_city_emit_crossstreet lays at road level
         * (framedump top-down spans 178 / 206, seed 99991). Dropping the slab on
         * the mouth side leaves the carriageway flush and the corner pavement
         * arms (tg_block_emit_intersection) turn the sidewalk down the side
         * street instead. The built side keeps its pavement. */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_XSTOP") &&
            !tg_facade_built(h->si, s))
            continue;
        tg_city_edge_frame(h->nl, h->si, sg, e);

        /* Top slab: near-in, near-out, far-out, far-in. */
        q[0] = e[0];              q[1]  = e[1] + TD5_TG_KERB_H; q[2]  = e[2];
        q[3] = e[0] + e[6] * sw;  q[4]  = e[1] + TD5_TG_KERB_H; q[5]  = e[2] + e[7] * sw;
        q[6] = e[3] + e[8] * sw;  q[7]  = e[4] + TD5_TG_KERB_H; q[8]  = e[5] + e[9] * sw;
        q[9] = e[3];              q[10] = e[4] + TD5_TG_KERB_H; q[11] = e[5];
        t[0] = 0.0; t[1] = (double)h->si;
        t[2] = u_w; t[3] = (double)h->si;
        t[4] = u_w; t[5] = (double)h->si + 1.0;
        t[6] = 0.0; t[7] = (double)h->si + 1.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);

        /* Kerb face, road-facing: bottom sits ON the asphalt so there is no
         * lip between the two, top meets the slab. */
        q[0] = e[0]; q[1]  = e[1];                 q[2]  = e[2];
        q[3] = e[0]; q[4]  = e[1] + TD5_TG_KERB_H; q[5]  = e[2];
        q[6] = e[3]; q[7]  = e[4] + TD5_TG_KERB_H; q[8]  = e[5];
        q[9] = e[3]; q[10] = e[4];                 q[11] = e[5];
        t[0] = 0.0; t[1] = (double)h->si;
        t[2] = u_k; t[3] = (double)h->si;
        t[4] = u_k; t[5] = (double)h->si + 1.0;
        t[6] = 0.0; t[7] = (double)h->si + 1.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    }

    if (n <= 0) return 1;
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    tg_acct_n(TG_ACCT_SIDEWALK, h->si, n / 8);   /* slab + kerb per side */
    h->moff[(*h->nmesh)++] = h->blk->len;
    return tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                              &seg_page, &seg_nq, 1);
}

/* FLAT VERGE BAND -- the out-of-town "sidewalk" (item 7). One quad per side
 * lying on the ground beside the road, on the same paving page as the city
 * pavement, with no kerb face and no rise: outside a town a made-up margin is
 * paint and gravel, not a slab, and a raised lip out there would only be
 * something to trip a car that runs wide.
 *
 * The lift is 16 raw, chosen the same way TD5_TG_CROSS_LIFT was: there is no
 * polygon-offset path, the ground skirt sits 70 raw BELOW road level, so
 * anything in between wins the depth test without reading as a step. */
static int tg_city_emit_verge_band(const TG_FBHook *h, double bw)
{
    double px[8], py[8], pz[8], uu[8], vv[8];
    double e[10], q[12], t[8];
    const double u_w = bw / (double)TD5_TG_SPAN_LENGTH;
    int seg_page = TD5_TG_PAGE_SIDEWALK, seg_nq;
    int s, n = 0;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        if (tg_pavement_side_blocked(h->nl, h->si, sg)) continue;
        tg_city_edge_frame(h->nl, h->si, sg, e);

        /* Same winding and the same isotropic UV as the city slab, so the two
         * tile identically where a biome changes mid-block. */
        q[0] = e[0];             q[1]  = e[1] + TD5_TG_VERGE_LIFT; q[2]  = e[2];
        q[3] = e[0] + e[6] * bw; q[4]  = e[1] + TD5_TG_VERGE_LIFT; q[5]  = e[2] + e[7] * bw;
        q[6] = e[3] + e[8] * bw; q[7]  = e[4] + TD5_TG_VERGE_LIFT; q[8]  = e[5] + e[9] * bw;
        q[9] = e[3];             q[10] = e[4] + TD5_TG_VERGE_LIFT; q[11] = e[5];
        t[0] = 0.0; t[1] = (double)h->si;
        t[2] = u_w; t[3] = (double)h->si;
        t[4] = u_w; t[5] = (double)h->si + 1.0;
        t[6] = 0.0; t[7] = (double)h->si + 1.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    }

    if (n <= 0) return 1;
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    tg_acct_n(TG_ACCT_SIDEWALK, h->si, n / 4);   /* out-of-town verge band */
    h->moff[(*h->nmesh)++] = h->blk->len;
    return tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                              &seg_page, &seg_nq, 1);
}

/* PEDESTRIAN RAILING along the KERB: one alpha-keyed plane per side, scenery is
 * submitted CULL_NONE so a single plane reads from both sides. "most of the
 * times" in the feedback, not always -- a hash gate drops roughly a third of
 * spans, which is what breaks the railing into runs that end at crossings and
 * side streets instead of ringing the whole city.
 *
 * SIDE (2026-08-27, "fences should be on the side near the road"). The railing
 * used to stand at 0.88 of the pavement width, i.e. hard against the building
 * line, which reads as a fence around each property rather than street
 * furniture. A pedestrian guard rail exists to keep people OFF the carriageway,
 * so it belongs at the kerb: TD5_TG_FENCE_KERB back from the kerb face, far
 * enough that a car clipping the kerb does not pass through it and the posts
 * still stand on the slab. */
static int tg_city_emit_fence(const TG_FBHook *h, double sw)
{
    double px[8], py[8], pz[8], uu[8], vv[8];
    double e[10], q[12], t[8];
    /* One page per span across, so the upright pitch is span-independent. */
    const double u_n = 1.0;
    int seg_page = TD5_TG_PAGE_FENCE, seg_nq;
    int s, n = 0;
    /* [R3 item 7] The kerb railing should read as a CONTINUOUS street edge and
     * break ONLY where it has a reason to: a pedestrian crossing over the road
     * (both kerbs), this side opening onto a side street (a facade gap is a
     * street mouth in this generator's block model), or a biome-run boundary
     * where the city itself ends. The old code dropped ~1/4 of spans through an
     * independent per-side hash (`(fh >> 29) >= 6`), which read as a railing
     * full of random holes rather than a street edge -- the user's report. That
     * hash is gone; the breaks below are the only ones now. */
    const int crossing = tg_city_crossing_here(h->si);
    const int biome_edge = (h->si > 0 &&
        tg_biome_cell_index(h->si) != tg_biome_cell_index(h->si - 1));

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        /* Kerb-side, clamped so a narrow pavement still puts it inboard of the
         * building line rather than off the far edge of the slab. */
        const double back = (sw * 0.35 < TD5_TG_FENCE_KERB)
                          ? sw * 0.35 : TD5_TG_FENCE_KERB;
        if (crossing) continue;                       /* pedestrians cross here */
        if (!tg_facade_built(h->si, s)) continue;     /* side-street mouth */
        if (biome_edge) continue;                     /* city/biome edge */
        if (tg_side_blocked(h->si, sg)) continue;
        tg_city_edge_frame(h->nl, h->si, sg, e);

        q[0] = e[0] + e[6] * back; q[1]  = e[1] + TD5_TG_KERB_H;
        q[2] = e[2] + e[7] * back;
        q[3] = e[3] + e[8] * back; q[4]  = e[4] + TD5_TG_KERB_H;
        q[5] = e[5] + e[9] * back;
        q[6] = e[3] + e[8] * back; q[7]  = e[4] + TD5_TG_KERB_H + TD5_TG_FENCE_H;
        q[8] = e[5] + e[9] * back;
        q[9] = e[0] + e[6] * back; q[10] = e[1] + TD5_TG_KERB_H + TD5_TG_FENCE_H;
        q[11] = e[2] + e[7] * back;
        /* v = 1 at the base, matching the page's top-down row order. */
        t[0] = 0.0; t[1] = 1.0;
        t[2] = u_n; t[3] = 1.0;
        t[4] = u_n; t[5] = 0.0;
        t[6] = 0.0; t[7] = 0.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    }

    if (n <= 0) return 1;
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    tg_acct_n(TG_ACCT_FENCE, h->si, n / 4);      /* one railing per side */
    h->moff[(*h->nmesh)++] = h->blk->len;
    return tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                              &seg_page, &seg_nq, 1);
}

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

/* The crossing predicate WITHOUT the min-spacing thinning: the FIRST span of a
 * non-park side-street gap on either kerb, off forks and bridge approaches.
 * Both sides are tested, so a street opening on the left gets one too. */
static int tg_crossing_base(int si)
{
    int s;
    if (si <= 1) return 0;
    /* Over a fork the main carriageway is HALF width and shifted, so a
     * kerb-to-kerb quad would float across the gore. The pavements survive it
     * (the shifted half road's outer edge is still the full-width edge) but a
     * crossing spans both edges, so skip the whole fork region. */
    if (tg_branches_enabled() && tg_span_in_fork_clear(si)) return 0;
    /* [R6 CROSS item 15] no crossing right after (or before) a bridge run. */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE") &&
        tg_span_near_bridge(si, TD5_TG_XBRIDGE_CLEAR)) return 0;
    for (s = 0; s < 2; s++) {
        if (!(!tg_facade_built(si, s) && tg_facade_built(si - 1, s))) continue;
        /* [R4 CROSS item 4] A zebra marks a road you cross INTO A STREET. On
         * seed 99991 one gap in four is a PARK (a green lawn + hedge from the
         * kerb out, tg_block_is_park), and painting a pedestrian crossing in
         * front of it is exactly the "weird green texture on some crossings"
         * report: the crosswalk reads as leading into a bright-green wall. A
         * park frontage is not a through street, so it gets no crossing. The
         * side-street mouths that ARE through streets still do. */
        if (tg_block_is_park(si, s)) continue;
        return 1;
    }
    return 0;
}

/* Where a pedestrian crossing is actually painted: a base crossing span that
 * survives the min-spacing thinning. The thinning is a greedy left-to-right
 * keep/drop simulated over a bounded backward window, so the decision is a pure
 * function of si with no cross-call state (this is called from both the zebra
 * emitter and the kerb-fence break gate, possibly out of span order). */
static int tg_city_crossing_here(int si)
{
    int j, last = -1000000;
    if (!tg_crossing_base(si)) return 0;
    /* DEFAULT ON; TD5RE_AUTOTRACK_XMIN=0 restores the un-thinned behaviour. */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_XMIN")) return 1;
    for (j = si - TD5_TG_XMIN_LOOK; j < si; j++) {
        if (j > 1 && tg_crossing_base(j) && j - last >= TD5_TG_XMIN_GAP)
            last = j;                 /* j is a KEPT crossing */
    }
    return si - last >= TD5_TG_XMIN_GAP;
}

/* ZEBRA CROSSING: one flat quad lying on the road, kerb to kerb.
 *
 * Two conventions copied from tg_emit_road_quad, which this has to sit on top
 * of without fighting it: the corners come from tg_road_edge (so the crossing
 * follows the same curvature and camber as the asphalt under it) and u runs
 * 0..lanes across the road. That makes the page tile once per lane, and since
 * the page carries two bars, a 4-lane road gets 8 bars -- bars running ALONG
 * travel, which is how a crossing is actually painted.
 *
 * Z-FIGHTING: the renderer has no polygon-offset path, so the only lever is a
 * lift. TD5_TG_CROSS_LIFT is 20 raw (~0.08 wu) -- far less than the 70 the
 * ground skirt drops by, enough to win the depth test at the distances a
 * crossing is visible from, and too small to see as a step. */
static int tg_city_emit_crossing(const TG_FBHook *h)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    double l0x, l0y, l0z, r0x, r0y, r0z;
    double l1x, l1y, l1z, r1x, r1y, r1z;
    const double f0 = 0.22, f1 = 0.62;    /* ~600 raw of crossing, in-span */
    const double L = (double)h->lanes;
    int seg_page = TD5_TG_PAGE_CROSSING, seg_nq = 1;
    int n = 0;

    /* [R5 CROSS item 12] the zebra reads correctly only on tarmac; skip it on
     * the pale gravel/cobble surfaces where it clashes (the side-street mouth
     * still draws, on the biome road page). */
    if (!tg_span_surface_is_tarmac(h->si)) return 1;

    tg_road_edge(h->nl, h->si, f0, 0.0, 1.0, &l0x, &l0y, &l0z, &r0x, &r0y, &r0z);
    tg_road_edge(h->nl, h->si, f1, 0.0, 1.0, &l1x, &l1y, &l1z, &r1x, &r1y, &r1z);

    px[n]=r0x; py[n]=r0y+TD5_TG_CROSS_LIFT; pz[n]=r0z; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=l0x; py[n]=l0y+TD5_TG_CROSS_LIFT; pz[n]=l0z; uu[n]=L;   vv[n]=0.0; n++;
    px[n]=l1x; py[n]=l1y+TD5_TG_CROSS_LIFT; pz[n]=l1z; uu[n]=L;   vv[n]=1.0; n++;
    px[n]=r1x; py[n]=r1y+TD5_TG_CROSS_LIFT; pz[n]=r1z; uu[n]=0.0; vv[n]=1.0; n++;

    if (*h->nmesh >= h->maxmesh) return 1;
    tg_acct(TG_ACCT_CROSSING, h->si);
    h->moff[(*h->nmesh)++] = h->blk->len;
    {   /* [R8 GUARD] A zebra is authored KERB TO KERB, so the on-road guard's
         * coverage test would drop it on sight. It is marked DECAL instead of
         * exempt: covering the road is licensed, but ONLY flush with the tarmac
         * (this quad is lifted TD5_TG_CROSS_LIFT = 20 raw). A crossing slab that
         * floats above the road is still rejected, which is R8 item 3 stated as
         * a rule rather than as a special case. */
        const size_t d0 = h->blk->len;
        int r = tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                   &seg_page, &seg_nq, 1);
        tg_guard_mark(d0, h->blk->len, TG_GK_DECAL, h->si);
        return r;
    }
}

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

static int tg_city_emit_lamp(const TG_FBHook *h, double sw)
{
    const TG_Node *n = &h->nl->v[h->si];
    /* Post stands on the pavement a little back from the kerb face, so a car
     * clipping the kerb does not visually pass through it. */
    const double stand = (sw > 0.0) ? sw * 0.35 : 300.0;
    /* Biome-aware kerb height (r2-city item 7): outside a town the verge is
     * flat paint, not a slab, so the post stands on the ground rather than a
     * step that is not there. Supersedes the fixed TD5_TG_KERB_H this used. */
    const double base_y = n->y + tg_city_kerb_h(h->b);
    /* Where the lantern ends up, relative to the post: the arm's far end. */
    const double reach = TD5_TG_LAMP_W * (1.0 - TD5_TG_LAMP_POST_U);
    int s;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        const double lx = n->tz * sg, lz = -n->tx * sg;   /* outward unit */
        const double post = n->width * 0.5 + stand;
        /* Quad spans the lateral axis: outer edge (u = 0) sits outboard of the
         * post by the page's own margin, inner edge (u = LAMP_U) hangs over the
         * road, so the head really is above the carriageway. */
        const double o = post + TD5_TG_LAMP_W * TD5_TG_LAMP_POST_U;
        const double in = o - TD5_TG_LAMP_W;
        double px[4], py[4], pz[4], uu[4], vv[4];
        int seg_page = TD5_TG_PAGE_LAMPPOST, seg_nq = 1;

        if (tg_side_blocked(h->si, sg)) continue;
        if (*h->nmesh + 2 > h->maxmesh) return 1;

        /* Quad loop: outer-bottom, inner-bottom, inner-top, outer-top. v = 1 at
         * the base, matching the page's top-down row order. */
        px[0] = n->x + lx * o; py[0] = base_y;                  pz[0] = n->z + lz * o;
        px[1] = n->x + lx * in; py[1] = base_y;                 pz[1] = n->z + lz * in;
        px[2] = n->x + lx * in; py[2] = base_y + TD5_TG_LAMP_H; pz[2] = n->z + lz * in;
        px[3] = n->x + lx * o; py[3] = base_y + TD5_TG_LAMP_H;  pz[3] = n->z + lz * o;
        uu[0] = 0.0;            vv[0] = 1.0;
        uu[1] = TD5_TG_LAMP_U;  vv[1] = 1.0;
        uu[2] = TD5_TG_LAMP_U;  vv[2] = 0.0;
        uu[3] = 0.0;            vv[3] = 0.0;

        h->moff[(*h->nmesh)++] = h->blk->len;
        if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, 4,
                                &seg_page, &seg_nq, 1))
            return 0;
        tg_acct(TG_ACCT_LAMP, h->si);
        /* GLOW at the lantern: tg_prop_one places PP_LAMP at y + its own y_off
         * (2500 == TD5_TG_LAMP_H) and measures `gap` from the road edge, so the
         * arm's reach comes off the stand to land the glow on the head. */
        if (!tg_prop_one(h->nl, h->si, PP_LAMP, sg,
                         stand - reach, h->blk, h->moff, h->nmesh))
            return 0;
    }
    return 1;
}

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

/* One BACKGROUND building written as its own mesh. `solid` closes it into a BOX
 * (front grid + roof + two side returns + a back sheet) instead of the old bare
 * front grid. The bare grid is a zero-thickness sheet: seen from anything but
 * dead-on -- which is most of a curving street and the whole fork gore -- it read
 * as a paper sliver, the "background buildings ... look wrong" (item 3) and part
 * of "buildings ... only a facade" (item 15). Scenery is submitted CULL_NONE, so
 * every face reads from both sides and the winding only has to be self-consistent.
 * `bx,by,bz` is the front-left base, `a*` the along-frontage vector to the
 * front-right base, `l*0`/`l*1` the OUTWARD lateral units at the two ends and
 * `depth` how far back the box goes. Returns 0 only on a buffer write failure; a
 * full mesh table is a silent no-op success, matching the back-row contract. */
static int tg_bg_building_box(TG_Buf *blk, size_t *moff, int *nmesh, int maxmesh,
                              double bx, double by, double bz,
                              double ax, double ay, double az,
                              double lx0, double lz0, double lx1, double lz1,
                              double depth, double H, int cols, int rows,
                              int page, int solid, int si)
{
    double px[TD5_TG_FACADE_MAXQUAD * 4], py[TD5_TG_FACADE_MAXQUAD * 4];
    double pz[TD5_TG_FACADE_MAXQUAD * 4], uu[TD5_TG_FACADE_MAXQUAD * 4];
    double vv[TD5_TG_FACADE_MAXQUAD * 4];
    int seg_nq, n = 0;

    tg_facade_push_grid(bx, by, bz, ax, ay, az, 0.0, H, 0.0,
                        cols, rows, 0, rows, px, py, pz, uu, vv, &n);
    if (solid && depth > 1.0) {
        const double blx = bx + lx0 * depth, blz = bz + lz0 * depth;
        const double frx = bx + ax, fry = by + ay, frz = bz + az;
        const double brx = frx + lx1 * depth, brz = frz + lz1 * depth;
        double q[12];
        /* ROOF (matches the front-facade MASS roof winding). */
        q[0] = bx;  q[1] = by + H;       q[2] = bz;
        q[3] = frx; q[4] = fry + H;      q[5] = frz;
        q[6] = brx; q[7] = fry + H;      q[8] = brz;
        q[9] = blx; q[10] = by + H;      q[11] = blz;
        tg_facade_push_quad(q, px, py, pz, uu, vv, &n);
        /* LEFT return. */
        q[0] = bx;  q[1] = by;           q[2] = bz;
        q[3] = blx; q[4] = by;           q[5] = blz;
        q[6] = blx; q[7] = by + H;       q[8] = blz;
        q[9] = bx;  q[10] = by + H;      q[11] = bz;
        tg_facade_push_quad(q, px, py, pz, uu, vv, &n);
        /* RIGHT return. */
        q[0] = frx; q[1] = fry;          q[2] = frz;
        q[3] = brx; q[4] = fry;          q[5] = brz;
        q[6] = brx; q[7] = fry + H;      q[8] = brz;
        q[9] = frx; q[10] = fry + H;     q[11] = frz;
        tg_facade_push_quad(q, px, py, pz, uu, vv, &n);
        /* BACK sheet. */
        q[0] = blx; q[1] = by;           q[2] = blz;
        q[3] = brx; q[4] = fry;          q[5] = brz;
        q[6] = brx; q[7] = fry + H;      q[8] = brz;
        q[9] = blx; q[10] = by + H;      q[11] = blz;
        tg_facade_push_quad(q, px, py, pz, uu, vv, &n);
    }
    if (n <= 0) return 1;
    if (*nmesh >= maxmesh) return 1;
    seg_nq = n / 4;
    moff[(*nmesh)++] = blk->len;
    if (!tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, &page, &seg_nq, 1))
        return 0;
    tg_acct(TG_ACCT_BUILDING, si);
    return 1;
}

static int tg_city_emit_backrows(const TG_FBHook *h, double sw)
{
    const TG_Biome *b = h->b;
    const TG_Node *n0 = &h->nl->v[h->si];
    const TG_Node *n1;
    int s, r;

    if (h->si + 1 >= h->nl->count) return 1;
    n1 = &h->nl->v[h->si + 1];

    /* Default ON (gated to real streets); TD5RE_AUTOTRACK_BACKROW_STREETS=0
     * restores the old "a row behind every span, both sides" behaviour for an
     * A/B -- that is the state the round-3 report was taken against. */
    const int gate = td5_env_flag_on("TD5RE_AUTOTRACK_BACKROW_STREETS");

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        const double lx0 = n0->tz * sg, lz0 = -n0->tx * sg;
        const double lx1 = n1->tz * sg, lz1 = -n1->tx * sg;
        unsigned int blk, ph, gs, gl;
        int av, nrow;
        if (tg_side_blocked(h->si, sg)) continue;
        /* GATE to a street opening. A solid frontage hides whatever is behind
         * it, so a back row there only ever pokes above the front roofline --
         * the "rows behind rows" everywhere. tg_facade_built == 0 on this side is
         * the gap the cross-street asphalt (tg_city_emit_crossstreet) runs
         * through, and the ONLY place the block behind is actually visible. */
        if (gate && tg_facade_built(h->si, s)) continue;
        /* Street WIDTH CLASS (item 2). An avenue opens both kerbs and carries
         * traffic, so it reveals a deeper, taller block receding; a narrow
         * pedestrian side street reveals a single closer, lower row. Read-only
         * use of the same partition the frontage and crossings key off, so all
         * three agree on which openings are avenues. */
        tg_facade_block(h->si, s, &blk, &ph, &gs, &gl, &av);
        nrow = (gate && !av) ? 1 : TD5_TG_BACKROW_N;
        for (r = 0; r < nrow; r++) {
            /* Row hash: per span, per side, per row -- so neighbouring spans
             * step in height and the two rows never line up. */
            const unsigned int rh = ((unsigned)h->si * 31u + (unsigned)s * 7u
                                     + (unsigned)r) * 2246822519u;
            double set, H, ax, az, ay, bx, by, bz, flen;
            int rows, cols, page;

            if ((rh >> 29) == 0u) continue;      /* ~12% of slots left empty */
            /* Each row sits a building depth plus clear air behind the last. */
            set = sw + tg_facade_depth(b) * (double)(r + 1)
                + TD5_TG_BACKROW_GAP * (double)(r + 1)
                + (double)(rh % 1800u);
            rows = b->floors_min + (int)((rh >> 9) % 5u) + ((gate && av) ? 1 : 0);
            H    = (double)rows * tg_facade_floor_h(b);

            bx = n0->x + lx0 * (n0->width * 0.5 + set);
            by = n0->y;
            bz = n0->z + lz0 * (n0->width * 0.5 + set);
            ax = (n1->x + lx1 * (n1->width * 0.5 + set)) - bx;
            ay = n1->y - n0->y;
            az = (n1->z + lz1 * (n1->width * 0.5 + set)) - bz;

            /* Columns from the row's OWN frontage length, which at a big setback
             * on a curve is appreciably longer than a span: the fixed 2 columns
             * this used squashed the page to 750 raw across, a third of its
             * authored 2150, and the back rows read as a different (much finer)
             * building than the front row of the same block. */
            flen = sqrt(ax * ax + az * az);
            cols = tg_facade_cols_for(flen, (double)b->cell_w, 4);
            page = tg_facade_page_class(rh, rows);
            /* Solid box (item 3): the old flat grid read as a paper sliver from
             * any oblique angle. Default ON; TD5RE_AUTOTRACK_BACKROW_SOLID=0
             * restores the flat sheet for an A/B. */
            if (!tg_bg_building_box(h->blk, h->moff, h->nmesh, h->maxmesh,
                                    bx, by, bz, ax, ay, az, lx0, lz0, lx1, lz1,
                                    tg_facade_depth(b), H, cols, rows, page,
                                    td5_env_flag_on("TD5RE_AUTOTRACK_BACKROW_SOLID"),
                                    h->si))
                return 0;
        }
    }
    return 1;
}

/* ===================== [R3 BLOCK] SIDE-STREET DIRECTION =====================
 * (feedback R3 item 3). Shared by the cross-street carriageway and the sidewalk/
 * railing arms that flank it, so all three leave the kerb in the SAME direction.
 * A diagonal carriageway with square-on pavements would read as broken geometry;
 * routing every arm's outward direction through one function keeps them aligned.
 *
 * DIAGONAL variance ("investigate if the crossings can be diagonal"): the angle
 * is keyed on the facade SUPERBLOCK, not the span, so every span of one gap
 * skews by the same amount and the arm stays one straight street rather than a
 * fan. Roughly a third of junctions are angled; the rest stay square. The angle
 * is capped well under 45 deg so a leaning arm can never double back across the
 * main carriageway (which would put a side street on top of the road).
 * ========================================================================== */
#define TD5_TG_DIAG_MAX_DEG   28.0    /* steepest diagonal side street, deg */

static double tg_block_arm_skew(int si, int left)
{
    unsigned int block, phase, gs, gl, h;
    int av;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_DIAG_CROSS")) return 0.0;
    tg_facade_block(si, left, &block, &phase, &gs, &gl, &av);
    h = (block * 2u + (unsigned)(left ? 1 : 0)) * 2654435761u;
    if ((h >> 28) >= 5u) return 0.0;             /* ~31% of gaps run angled */
    {   /* signed fraction in -1..1 from an independent hash slice */
        const double f = (double)((h >> 8) & 0xFFFFu) / 32768.0 - 1.0;
        return f * (TD5_TG_DIAG_MAX_DEG * TD5_TG_PI / 180.0);
    }
}

/* Rotate an outward unit (ux,uz) in the XZ plane by `ang` radians. */
static void tg_block_rot2(double ux, double uz, double ang,
                          double *ox, double *oz)
{
    const double c = cos(ang), s = sin(ang);
    *ox = ux * c - uz * s;
    *oz = ux * s + uz * c;
}

/* A park gap carries a lawn instead of a carriageway, so the crossstreet emitter
 * skips it. Defined with the park code further down; forward-declared here. */
static int tg_block_is_park(int si, int left);

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
static double tg_city_crossst_reach(const TG_Biome *b, double sw)
{
    const double gaps = td5_env_flag_on("TD5RE_AUTOTRACK_XLONG")
                      ? TD5_TG_XSTREET_GAPS : 1.0;
    return sw + tg_facade_depth(b) + TD5_TG_BACKROW_GAP * gaps;
}

static int tg_city_emit_crossstreet(const TG_FBHook *h, double sw)
{
    double px[8], py[8], pz[8], uu[8], vv[8];
    double e[10], q[12], t[8];
    const double reach = tg_city_crossst_reach(h->b, sw);
    const double drop  = TD5_TG_GROUND_DROP * reach / TD5_TG_GROUND_WIDTH;
    const double u_r   = reach / (double)TD5_TG_LANE_WIDTH;
    /* [R4 CROSS item 9] Perpendicular lane markings: swap in the cross-street
     * page whose centre line runs DOWN the street (see tg_emit_texture_page_r4_cross).
     * Default ON; TD5RE_AUTOTRACK_CROSS_MARKINGS=0 restores the biome road page. */
    const int marks = td5_env_flag_on("TD5RE_AUTOTRACK_CROSS_MARKINGS");
    int seg_page = marks ? (TD5_TG_PAGE_R4_CROSS + 0) : tg_road_page(h->si), seg_nq;
    int s, n = 0;

    /* [R6 CROSS item 15] no cross-street carriageway right after a bridge run. */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE") &&
        tg_span_near_bridge(h->si, TD5_TG_XBRIDGE_CLEAR)) return 1;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        /* Only where THIS kerb is open. On an avenue both are, and the two
         * quads plus the crossing between them make one crossroads. */
        double ang, nox, noz, fox, foz;
        if (tg_facade_built(h->si, s)) continue;
        if (tg_block_is_park(h->si, s)) continue;   /* a park, not a through st */
        if (tg_side_blocked(h->si, sg)) continue;
        tg_city_edge_frame(h->nl, h->si, sg, e);

        /* Skew the outward edge (item 3 diagonal). Both ends rotate by the same
         * per-gap angle, so the carriageway leans as one straight street. */
        ang = tg_block_arm_skew(h->si, s);
        tg_block_rot2(e[6], e[7], ang, &nox, &noz);
        tg_block_rot2(e[8], e[9], ang, &fox, &foz);

        q[0] = e[0];                q[1]  = e[1] + TD5_TG_VERGE_LIFT;
        q[2] = e[2];
        q[3] = e[0] + nox * reach;  q[4]  = e[1] + TD5_TG_VERGE_LIFT - drop;
        q[5] = e[2] + noz * reach;
        q[6] = e[3] + fox * reach;  q[7]  = e[4] + TD5_TG_VERGE_LIFT - drop;
        q[8] = e[5] + foz * reach;
        q[9] = e[3];                q[10] = e[4] + TD5_TG_VERGE_LIFT;
        q[11] = e[5];
        t[0] = 0.0; t[1] = (double)h->si;
        t[2] = u_r; t[3] = (double)h->si;
        t[4] = u_r; t[5] = (double)h->si + 1.0;
        t[6] = 0.0; t[7] = (double)h->si + 1.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    }

    if (n <= 0) return 1;
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    tg_acct_n(TG_ACCT_CROSSING, h->si, n / 4);   /* side-street mouths */
    if (marks) tg_acct_n(TG_ACCT_CROSSFURN, h->si, n / 4);  /* item 9 markings fired */
    h->moff[(*h->nmesh)++] = h->blk->len;
    return tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                              &seg_page, &seg_nq, 1);
}

/* ==========================================================================
 * [R3 BLOCK] STREET INTERSECTIONS, PARKS & HOUSES  (feedback R3 items 3-6)
 *
 * This whole block is owned by the BLOCK work area. It sits AFTER Group A (city
 * furniture) and BEFORE the Group A dispatcher on purpose -- its own dispatcher
 * tg_emit_fb_block is wired into the scenery loop next to tg_emit_fb_city, so
 * none of the code below edits another area's emitter.
 *
 * WHAT A "GAP" IS. tg_facade_built(si, side) is false where a superblock's side
 * street opens the frontage. Round 2 filled that gap with a carriageway arm
 * (tg_city_emit_crossstreet) but the pavements and railings ran straight past
 * it, so a "crossing" was a hole in a wall, not a junction. Two things fix that:
 *   1. INTERSECTIONS (item 3): at the two ALONG-ROAD corners of a gap, a
 *      pavement + railing turn off the main kerb and run OUTWARD down the side
 *      street, so the sidewalk and the guard rail follow the street. The arm
 *      shares its outward direction with the carriageway (tg_block_arm_skew), so
 *      a diagonal side street gets diagonal pavements too.
 *   2. PARKS (items 5-6): one gap in four is a green square instead of a through
 *      street -- a lawn from the kerb out, a hedge border behind the pavement,
 *      and the occasional individual house set back on the grass. A park and a
 *      side street are mutually exclusive on a given gap (tg_block_is_park is the
 *      single predicate both the crossstreet guard and the park emitter read),
 *      so a lawn never lands on top of a carriageway.
 * ========================================================================== */

/* Is the gap on span si / side `left` a PARK rather than a through street?
 * Keyed on the facade SUPERBLOCK so every span of one gap agrees (a gap lies
 * wholly within one block's period). Avenues -- open on both kerbs -- are never
 * parks: an avenue is a through route, not a square.
 *
 * [R6 CROSS item 2] DEFAULT OFF. Every park IS a roadside gap, so from the
 * racing line a park reads as a side street whose surface is a green lawn with
 * the boundary hedge standing across the far end like a guardrail -- verbatim
 * "an intersection where instead of road I see green grass ... it still has that
 * vertical green texture as guardrail" (framedump top-down span 115, seed 99991:
 * a bright-green mouth on the right kerb). The user's instruction is explicit --
 * "fix this to be street" -- so parks no longer open a gap: with is_park false a
 * former park gap is laid as a normal through street (carriageway + crossing +
 * lined frontages) by the existing emitters, and no lawn or hedge is emitted.
 * TD5RE_AUTOTRACK_PARKS=1 restores the green squares for an A/B. */
static int tg_block_is_park(int si, int left)
{
    unsigned int block, phase, gs, gl, h;
    int av;
    if (!td5_env_int("TD5RE_AUTOTRACK_PARKS", 0, 0, 1)) return 0;
    tg_facade_block(si, left, &block, &phase, &gs, &gl, &av);
    if (av) return 0;
    h = (block * 2u + (unsigned)(left ? 1 : 0)) * 0x85EBCA6Bu;
    return (h >> 30) == 0u;                       /* ~1 gap in 4 is a park */
}

/* [R7 CITY item 3] Ground + dressing under a fork-gore BUILDING backdrop. The
 * backdrop massing (tg_bg_building_box) stood on its base chord alone; where the
 * far terrain does not reach behind the corridor the block read as "buildings at
 * the back that don't reach the floor / have no ground" (span 137, seed 99991).
 * This lays a tiled PLAZA slab from the carriageway clearance out past the block
 * back, at the block's own base Y (the road-node Y), so the buildings visibly
 * stand on a floor, and dresses it with a fountain/monument or a passer-by --
 * the "plazas can contain fountains and people" request. Everything sits BEHIND
 * the carriageway clearance (routes the same tg_carriageway_clear_gap the block
 * does, so the GUARD backstop has nothing to reject). Gate TD5RE_R7_CITY_PLAZA
 * (default ON); =0 restores the bare backdrop for an A/B. */
static int tg_city_emit_forkback_plaza(const TG_FBHook *h, double side,
                                       const TG_Node *n0, const TG_Node *n1,
                                       double lx0, double lz0,
                                       double lx1, double lz1,
                                       double set0, double set1, double depth)
{
    const int si = h->si;
    /* Inner edge sits at the skirt's outer reach (measured from the road edge),
     * underlapped a little so the plaza abuts the ordinary ground with no seam
     * and does NOT overlap-and-z-fight the sloping skirt inside it. The whole
     * point is the region BEYOND the 12000 skirt, where a fork backdrop stands
     * (front ~14000-21000 out, seed 99991 span 137) over the sunk far-band and so
     * "does not reach the floor". */
    const double inner = tg_verge_reach() - 1000.0;
    double dn, df, u_d;
    double px[4], py[4], pz[4], uu[4], vv[4];
    int seg_page = TD5_TG_PAGE_GROUND, seg_nq, pp;

    if (!td5_env_flag_on("TD5RE_R7_CITY_PLAZA")) return 1;

    dn  = n0->width * 0.5 + set0 + depth;      /* near outer = block back */
    df  = n1->width * 0.5 + set1 + depth;      /* far  outer              */
    u_d = (dn - (n0->width * 0.5 + inner)) / (double)TD5_TG_SPAN_LENGTH;
    {
        const double inx0 = n0->x + lx0 * (n0->width * 0.5 + inner);
        const double inz0 = n0->z + lz0 * (n0->width * 0.5 + inner);
        const double inx1 = n1->x + lx1 * (n1->width * 0.5 + inner);
        const double inz1 = n1->z + lz1 * (n1->width * 0.5 + inner);
        px[0]=inx0;            py[0]=n0->y; pz[0]=inz0;            uu[0]=0.0; vv[0]=(double)si;
        px[1]=n0->x+lx0*dn;    py[1]=n0->y; pz[1]=n0->z+lz0*dn;   uu[1]=u_d; vv[1]=(double)si;
        px[2]=n1->x+lx1*df;    py[2]=n1->y; pz[2]=n1->z+lz1*df;   uu[2]=u_d; vv[2]=(double)si + 1.0;
        px[3]=inx1;            py[3]=n1->y; pz[3]=inz1;            uu[3]=0.0; vv[3]=(double)si + 1.0;
    }
    if (*h->nmesh < h->maxmesh) {
        seg_nq = 1;
        h->moff[(*h->nmesh)++] = h->blk->len;
        if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
            return 0;
        tg_acct(TG_ACCT_R7_CITY, si);          /* the plaza floor */
    }

    /* Dressing: a fountain/monument on a ~1-in-3 beat, a passer-by otherwise,
     * out on the plaza between the skirt end and the block front. tg_prop_one
     * adds width/2 + gap itself and accounts each as a PROP. */
    pp = ((unsigned)si % 3u == 0u) ? PP_MONUMENT : PP_PERSON0;
    if (!tg_prop_one(h->nl, si, pp, side, inner + 0.4 * (set0 - inner),
                     h->blk, h->moff, h->nmesh))
        return 0;
    return 1;
}

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
#define TD5_TG_FORKBACK_GAP        7000.0 /* new: background air past the corridor */
#define TD5_TG_FORKBACK_GAP_TALL   5000.0 /* old (A/B): pre-R5 gap                  */
#define TD5_TG_FORKBACK_DEPTH      3000.0 /* new: how deep the background reads     */
#define TD5_TG_FORKBACK_DEPTH_TALL 6000.0 /* old (A/B): pre-R5 depth                */
#define TD5_TG_FORKBACK_MAX_ROWS   6      /* a backdrop, not a downtown tower       */
/* [R6 item 3] Per-span variation of the fork-back band. STAGGER pushes each
 * span's block to a different depth so the face is jagged and neighbouring spans
 * no longer share an edge -- distinct buildings with gaps ("breaks in the row"),
 * not one swept curtain. BASE lifts the whole band further back so it reads as a
 * distant skyline rather than a wall hugging the branch. */
#define TD5_TG_FORKBACK_STAGGER_BASE  2500.0
#define TD5_TG_FORKBACK_STAGGER_RANGE 9000u

static int tg_city_emit_forkback(const TG_FBHook *h)
{
    const TG_Biome *b = h->b;
    const TG_NodeList *nl = h->nl;
    const int si = h->si;
    const double side = -1.0;               /* forks only clear the RIGHT lateral */
    const int small = td5_env_flag_on("TD5RE_AUTOTRACK_FORKBACK_SMALL");
    const double gap = small ? TD5_TG_FORKBACK_GAP : TD5_TG_FORKBACK_GAP_TALL;
    const TG_Node *n0, *n1;
    double lx0, lz0, lx1, lz1, set0, set1, bx, by, bz, ax, ay, az;
    unsigned int blk, ph, gs, gl, bh, sh;
    const int vary = td5_env_flag_on("TD5RE_AUTOTRACK_FORKBACK_VARY");
    int av;

    if (!td5_env_flag_on("TD5RE_AUTOTRACK_FORK_BACKDROP")) return 1;
    if (!tg_branches_enabled() || !tg_span_in_fork_clear(si)) return 1;
    if (tg_span_in_bridge_run(si)) return 1;
    if (si + 1 >= nl->count) return 1;
    n0 = &nl->v[si]; n1 = &nl->v[si + 1];

    lx0 = n0->tz * side; lz0 = -n0->tx * side;
    lx1 = n1->tz * side; lz1 = -n1->tx * side;

    /* Distance from the road EDGE to the front of the band: the whole corridor
     * clearance (clear_gap) plus a background gap so the block stands well back.
     * [R5 items 6/7] The clearance is taken PER ENDPOINT -- clear_gap(si) for the
     * near base and clear_gap(si+1) for the far base -- not one clear_gap(si) for
     * both. The corridor BOWS: the branch's outer edge is further out at si+1
     * than at si over the widening half of a fork, so applying si's clearance to
     * the far base set the front INBOARD of the branch there, and the front chord
     * skewed against the branch it was meant to back. That is item 6 ("background
     * building colliding with the road" at 153) and item 7 ("stretched ...
     * diagonally to the branch road" at 160): the branch bows into a backdrop
     * laid to the near span's narrower reach. Per-endpoint clearance makes the
     * band follow the bow. Default ON; TD5RE_AUTOTRACK_FORKBACK_FOLLOW=0 restores
     * the single-set behaviour for an A/B. */
    set0 = tg_carriageway_clear_gap(nl, si, side, tg_city_sidewalk_w(b),
                                    TD5_TG_CARRIAGEWAY_MARGIN) + gap;
    set1 = td5_env_flag_on("TD5RE_AUTOTRACK_FORKBACK_FOLLOW")
         ? tg_carriageway_clear_gap(nl, si + 1, side, tg_city_sidewalk_w(b),
                                    TD5_TG_CARRIAGEWAY_MARGIN) + gap
         : set0;

    /* [R6 item 3] Per-span stagger: shift this span's whole block back by a
     * span-keyed amount so the row breaks into distinct, differently-set-back
     * buildings instead of one continuous wall. Applied to both endpoints so the
     * block stays parallel; the far background band closes the ground behind the
     * gaps a stagger opens between neighbouring spans. */
    sh = ((unsigned)si * 2654435761u) ^ 0x9e3779b9u;
    if (vary) {
        const double stag = TD5_TG_FORKBACK_STAGGER_BASE
                          + (double)((sh >> 17) % TD5_TG_FORKBACK_STAGGER_RANGE);
        set0 += stag; set1 += stag;
    }

    bx = n0->x + lx0 * (n0->width * 0.5 + set0);
    by = n0->y;
    bz = n0->z + lz0 * (n0->width * 0.5 + set0);
    ax = (n1->x + lx1 * (n1->width * 0.5 + set1)) - bx;
    ay = n1->y - n0->y;
    az = (n1->z + lz1 * (n1->width * 0.5 + set1)) - bz;

    tg_facade_block(si, 0, &blk, &ph, &gs, &gl, &av);
    bh = blk * 2654435761u;

    if (tg_block_is_park(si, 0)) {
        /* PARK: a green lawn over the bare tiles, deep enough to read as open
         * ground behind the branch. One flat quad, park-lawn page, isotropic UV
         * so it tiles the same on a curve. */
        double px[4], py[4], pz[4], uu[4], vv[4], q[12];
        const double d = small ? TD5_TG_FORKBACK_DEPTH : TD5_TG_FORKBACK_DEPTH_TALL;
        int seg_page = TD5_TG_PAGE_R3_BLOCK + 0, seg_nq, n = 0;
        const double u_d = d / (double)TD5_TG_SPAN_LENGTH;
        q[0] = bx;              q[1] = by;  q[2] = bz;
        q[3] = bx + lx0 * d;    q[4] = by;  q[5] = bz + lz0 * d;
        q[6] = bx + ax + lx1*d; q[7] = by + ay; q[8] = bz + az + lz1 * d;
        q[9] = bx + ax;         q[10] = by + ay; q[11] = bz + az;
        px[0]=q[0]; py[0]=q[1]; pz[0]=q[2];  uu[0]=0.0;  vv[0]=(double)si;
        px[1]=q[3]; py[1]=q[4]; pz[1]=q[5];  uu[1]=u_d;  vv[1]=(double)si;
        px[2]=q[6]; py[2]=q[7]; pz[2]=q[8];  uu[2]=u_d;  vv[2]=(double)si + 1.0;
        px[3]=q[9]; py[3]=q[10];pz[3]=q[11]; uu[3]=0.0;  vv[3]=(double)si + 1.0;
        n = 4;
        if (*h->nmesh >= h->maxmesh) return 1;
        seg_nq = n / 4;
        h->moff[(*h->nmesh)++] = h->blk->len;
        if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                &seg_page, &seg_nq, 1))
            return 0;
        tg_acct(TG_ACCT_FORKBACK, si);
        return 1;
    }

    /* BUILDINGS: a skyline block set behind the corridor. [R6 item 3] height is
     * keyed PER SPAN (not the superblock) so neighbouring blocks step up and down
     * -- a varied skyline, not one flat-topped wall. The district climb is
     * dropped for the backdrop: it only pinned the whole run to the cap, which is
     * what read as the uniform "line of skyscrapers". */
    {
        int rows = vary
                 ? b->floors_min + (int)((sh >> 11) % 5u)
                 : b->floors_min + tg_city_district_floors(blk)
                   + (int)((bh >> 9) % (small ? 3u : 5u)) + (small ? 0 : 2);
        const int cap = small ? TD5_TG_FORKBACK_MAX_ROWS : TD5_TG_FACADE_MAX_ROWS;
        const double depth = small ? TD5_TG_FORKBACK_DEPTH
                                   : TD5_TG_FORKBACK_DEPTH_TALL;
        double H, flen;
        int cols, page;
        if (rows > cap) rows = cap;
        H = (double)rows * tg_facade_floor_h(b);
        flen = sqrt(ax * ax + az * az);
        cols = tg_facade_cols_for(flen, (double)b->cell_w, 4);
        /* [R6 item 3] A backdrop uses a low-rise MASONRY page even when tall: the
         * glass TOWER pages (rows >= FACADE_TALL_ROWS) read as a blown-out white
         * slab at the distance a fork backdrop is usually seen from -- fork
         * 319-360 seen down the descending avenue from span 188 -- which is worse
         * than the uniform wall it replaced. Stone/brick reads correctly there. */
        page = vary ? tg_facade_page_class(bh, TD5_TG_FACADE_TALL_ROWS - 1)
                    : tg_facade_page_class(bh, rows);
        if (!tg_bg_building_box(h->blk, h->moff, h->nmesh, h->maxmesh,
                                bx, by, bz, ax, ay, az, lx0, lz0, lx1, lz1,
                                depth, H, cols, rows, page, 1, si))
            return 0;
        tg_acct(TG_ACCT_FORKBACK, si);
        /* [R7 item 3] give the backdrop a floor to stand on + plaza dressing. */
        if (!tg_city_emit_forkback_plaza(h, side, n0, n1, lx0, lz0, lx1, lz1,
                                         set0, set1, depth))
            return 0;
    }
    return 1;
}

/* ---- item 3: one corner arm -- a pavement + railing turning off the main kerb
 * to run OUTWARD along the side street. `C` is the kerb corner (a main-road edge
 * point); (ox,oz) the skewed OUTWARD unit down the street; (bx,bz) the BACK unit
 * (away from the gap, onto the built side) so the slab and railing sit clear of
 * the carriageway; `reach` how far the arm runs; `sw` the pavement width. Up to
 * 3 meshes: slab, its road-facing kerb face, and the railing. */
#define TD5_TG_ARM_SET   140.0    /* railing set back from the kerb, raw */

static int tg_block_emit_arm(const TG_FBHook *h,
                             double cx, double cy, double cz,
                             double ox, double oz, double bx, double bz,
                             double reach, double sw)
{
    double px[8], py[8], pz[8], uu[8], vv[8], q[12], t[8];
    const double drop = TD5_TG_GROUND_DROP * reach / TD5_TG_GROUND_WIDTH;
    const double ur = reach / (double)TD5_TG_SPAN_LENGTH;
    const double uw = sw / (double)TD5_TG_SPAN_LENGTH;
    const double back = (sw * 0.35 < TD5_TG_ARM_SET) ? sw * 0.35 : TD5_TG_ARM_SET;
    int seg_page, seg_nq, n;

    /* Slab top at kerb height: kerb-corner, kerb-outer, back-outer, back-corner.
     * "back" thickens the slab by sw away from the carriageway. */
    n = 0; seg_page = TD5_TG_PAGE_SIDEWALK;
    q[0]  = cx;                     q[1]  = cy + TD5_TG_KERB_H;
    q[2]  = cz;
    q[3]  = cx + ox * reach;        q[4]  = cy + TD5_TG_KERB_H - drop;
    q[5]  = cz + oz * reach;
    q[6]  = cx + ox * reach + bx * sw; q[7] = cy + TD5_TG_KERB_H - drop;
    q[8]  = cz + oz * reach + bz * sw;
    q[9]  = cx + bx * sw;           q[10] = cy + TD5_TG_KERB_H;
    q[11] = cz + bz * sw;
    t[0] = 0.0; t[1] = 0.0;  t[2] = ur; t[3] = 0.0;
    t[4] = ur;  t[5] = uw;   t[6] = 0.0; t[7] = uw;
    tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    tg_acct(TG_ACCT_SIDEWALK, h->si);
    h->moff[(*h->nmesh)++] = h->blk->len;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, &seg_page, &seg_nq, 1))
        return 0;

    /* Kerb face down to the carriageway along the kerb-corner..kerb-outer edge. */
    n = 0;
    q[0]  = cx;                q[1]  = cy;                    q[2]  = cz;
    q[3]  = cx + ox * reach;   q[4]  = cy - drop;             q[5]  = cz + oz * reach;
    q[6]  = cx + ox * reach;   q[7]  = cy + TD5_TG_KERB_H - drop;
    q[8]  = cz + oz * reach;
    q[9]  = cx;                q[10] = cy + TD5_TG_KERB_H;    q[11] = cz;
    t[0] = 0.0; t[1] = 0.0; t[2] = ur; t[3] = 0.0;
    t[4] = ur; t[5] = TD5_TG_KERB_H / (double)TD5_TG_SPAN_LENGTH;
    t[6] = 0.0; t[7] = TD5_TG_KERB_H / (double)TD5_TG_SPAN_LENGTH;
    tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    h->moff[(*h->nmesh)++] = h->blk->len;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, &seg_page, &seg_nq, 1))
        return 0;

    /* Railing along the same edge, set back onto the slab (posts off the road). */
    n = 0; seg_page = TD5_TG_PAGE_FENCE;
    q[0]  = cx + bx * back;              q[1]  = cy + TD5_TG_KERB_H;
    q[2]  = cz + bz * back;
    q[3]  = cx + bx * back + ox * reach; q[4]  = cy + TD5_TG_KERB_H - drop;
    q[5]  = cz + bz * back + oz * reach;
    q[6]  = cx + bx * back + ox * reach; q[7]  = cy + TD5_TG_KERB_H - drop + TD5_TG_FENCE_H;
    q[8]  = cz + bz * back + oz * reach;
    q[9]  = cx + bx * back;              q[10] = cy + TD5_TG_KERB_H + TD5_TG_FENCE_H;
    q[11] = cz + bz * back;
    t[0] = 0.0; t[1] = 1.0; t[2] = ur; t[3] = 1.0;
    t[4] = ur;  t[5] = 0.0; t[6] = 0.0; t[7] = 0.0;
    tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    tg_acct(TG_ACCT_FENCE, h->si);
    h->moff[(*h->nmesh)++] = h->blk->len;
    return tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, &seg_page, &seg_nq, 1);
}

/* Intersections (item 3): pavement + railing arms at the two along-road corners
 * of every side-street gap, so the sidewalks and guard rails TURN to follow the
 * street. Only at the gap's boundary spans (where the neighbour on that side is
 * built) -- the interior of the gap is the carriageway itself. Parks and forks
 * are skipped: a park has no through street, and a fork's half-width road has no
 * room for a junction. */
static int tg_block_emit_intersection(const TG_FBHook *h)
{
    double e[10];
    const double sw = tg_city_sidewalk_w(h->b);
    int s;

    if (!(sw > 0.0)) return 1;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_INTERSECTIONS")) return 1;
    if (tg_span_in_bridge_run(h->si)) return 1;
    if (td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE") &&   /* item 15 */
        tg_span_near_bridge(h->si, TD5_TG_XBRIDGE_CLEAR)) return 1;
    if (tg_branches_enabled() && tg_span_in_fork_clear(h->si)) return 1;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        double reach, ang, ax, az, alen, ox, oz;
        int near_corner, far_corner;

        if (tg_facade_built(h->si, s)) continue;      /* built = no junction */
        if (tg_block_is_park(h->si, s)) continue;     /* a park, not a street */
        if (tg_side_blocked(h->si, sg)) continue;
        near_corner = tg_facade_built(h->si - 1, s);  /* gap starts here */
        far_corner  = tg_facade_built(h->si + 1, s);  /* gap ends here */
        if (!near_corner && !far_corner) continue;    /* gap interior */

        tg_city_edge_frame(h->nl, h->si, sg, e);
        reach = tg_city_crossst_reach(h->b, sw);
        ang   = tg_block_arm_skew(h->si, s);
        /* Along-road unit, near -> far. */
        ax = e[3] - e[0]; az = e[5] - e[2];
        alen = sqrt(ax * ax + az * az);
        if (alen < 1e-6) { ax = 0.0; az = 1.0; } else { ax /= alen; az /= alen; }

        if (near_corner) {                            /* corner at the near node */
            tg_block_rot2(e[6], e[7], ang, &ox, &oz);
            /* back = -along (onto the built side, off the carriageway). */
            if (!tg_block_emit_arm(h, e[0], e[1], e[2], ox, oz, -ax, -az,
                                   reach, sw))
                return 0;
        }
        if (far_corner) {                             /* corner at the far node */
            tg_block_rot2(e[8], e[9], ang, &ox, &oz);
            if (!tg_block_emit_arm(h, e[3], e[4], e[5], ox, oz, ax, az,
                                   reach, sw))
                return 0;
        }
    }
    return 1;
}

/* ---- items 5-6: parks & houses ----------------------------------------------
 * A park fills a gap side with a lawn, a hedge border behind the pavement, and
 * the occasional house. The lawn abuts span to span (shared endpoints) into one
 * continuous green, the same no-pop trick the treeline band uses. */
#define TD5_TG_HEDGE_H    520.0    /* park hedge height, raw (~2 wu, see over)  */
#define TD5_TG_HOUSE_W   2600.0    /* individual house footprint, raw           */
#define TD5_TG_HOUSE_D   2200.0
#define TD5_TG_HOUSE_H   2600.0    /* ~two low storeys                          */

/* One small house box (4 walls + a roof) as a single 2-segment mesh, centred
 * along the span and set back on the lawn. Walls on the house page, roof on the
 * roof page. */
static int tg_block_emit_house(const TG_FBHook *h, const double *e, double set)
{
    double px[20], py[20], pz[20], uu[20], vv[20];
    double ax, az, alen, ox, oz, y0, drop;
    double flx, flz, frx, frz, blx, blz, brx, brz;
    const double w = TD5_TG_HOUSE_W, d = TD5_TG_HOUSE_D, H = TD5_TG_HOUSE_H;
    int seg_page[2], seg_nq[2], n = 0;

    /* Along-road unit and length of this span's edge. */
    ax = e[3] - e[0]; az = e[5] - e[2];
    alen = sqrt(ax * ax + az * az);
    if (alen < 1e-6) return 1;
    ax /= alen; az /= alen;
    ox = e[6]; oz = e[7];                     /* near outward (square-on house) */
    drop = TD5_TG_GROUND_DROP * set / TD5_TG_GROUND_WIDTH;
    y0 = e[1] + TD5_TG_VERGE_LIFT - drop;

    {   /* front-left base, centred along the span. */
        const double off = (alen - w) > 0.0 ? (alen - w) * 0.5 : 0.0;
        const double fx = e[0] + ax * off + ox * set;
        const double fz = e[2] + az * off + oz * set;
        flx = fx;            flz = fz;
        frx = fx + ax * w;   frz = fz + az * w;
        blx = flx + ox * d;  blz = flz + oz * d;
        brx = frx + ox * d;  brz = frz + oz * d;
    }

    /* Four walls (near-bottom, far-bottom, far-top, near-top per quad). */
    #define HQ(x0,z0,x1,z1) \
        do { \
            px[n]=x0; py[n]=y0;   pz[n]=z0; uu[n]=0.0; vv[n]=1.0; n++; \
            px[n]=x1; py[n]=y0;   pz[n]=z1; uu[n]=1.0; vv[n]=1.0; n++; \
            px[n]=x1; py[n]=y0+H; pz[n]=z1; uu[n]=1.0; vv[n]=0.0; n++; \
            px[n]=x0; py[n]=y0+H; pz[n]=z0; uu[n]=0.0; vv[n]=0.0; n++; \
        } while (0)
    HQ(flx, flz, frx, frz);      /* front */
    HQ(brx, brz, blx, blz);      /* back  */
    HQ(blx, blz, flx, flz);      /* left  */
    HQ(frx, frz, brx, brz);      /* right */
    #undef HQ

    /* Roof: flat deck at the top (near a low pitch would need a ridge; a deck
     * reads fine at the distance a park house is ever seen). */
    px[n]=flx; py[n]=y0+H; pz[n]=flz; uu[n]=0.0; vv[n]=1.0; n++;
    px[n]=frx; py[n]=y0+H; pz[n]=frz; uu[n]=1.0; vv[n]=1.0; n++;
    px[n]=brx; py[n]=y0+H; pz[n]=brz; uu[n]=1.0; vv[n]=0.0; n++;
    px[n]=blx; py[n]=y0+H; pz[n]=blz; uu[n]=0.0; vv[n]=0.0; n++;

    if (*h->nmesh >= h->maxmesh) return 1;
    seg_page[0] = TD5_TG_PAGE_R3_BLOCK + 2;   /* house wall */
    seg_nq[0]   = 4;
    seg_page[1] = TD5_TG_PAGE_R3_BLOCK + 3;   /* house roof */
    seg_nq[1]   = 1;
    tg_acct(TG_ACCT_HOUSE, h->si);
    h->moff[(*h->nmesh)++] = h->blk->len;
    return tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, seg_page, seg_nq, 2);
}

/* Park green + hedge (+ a house on some spans) for every open, non-street gap
 * side. Default ON; houses behind their own knob so a plain green park is one
 * A/B away. */
static int tg_block_emit_park(const TG_FBHook *h)
{
    const double sw = tg_city_sidewalk_w(h->b);
    int s;

    if (!(sw > 0.0)) return 1;
    if (tg_span_in_bridge_run(h->si)) return 1;
    if (tg_branches_enabled() && tg_span_in_fork_clear(h->si)) return 1;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        double e[10], q[12], t[8], px[8], py[8], pz[8], uu[8], vv[8];
        double reach, drop, u_r;
        int seg_page, seg_nq, n;

        if (tg_facade_built(h->si, s)) continue;
        if (!tg_block_is_park(h->si, s)) continue;    /* only park gaps */
        if (tg_side_blocked(h->si, sg)) continue;
        tg_city_edge_frame(h->nl, h->si, sg, e);

        reach = tg_city_crossst_reach(h->b, sw);
        drop  = TD5_TG_GROUND_DROP * reach / TD5_TG_GROUND_WIDTH;
        u_r   = reach / (double)TD5_TG_SPAN_LENGTH;

        /* Lawn: kerb -> reach, sinking with the skirt, isotropic UV so it tiles
         * the same on curves. Same winding as the verge band. */
        n = 0; seg_page = TD5_TG_PAGE_R3_BLOCK + 0;   /* park lawn */
        q[0] = e[0];               q[1]  = e[1] + TD5_TG_VERGE_LIFT;
        q[2] = e[2];
        q[3] = e[0] + e[6] * reach; q[4] = e[1] + TD5_TG_VERGE_LIFT - drop;
        q[5] = e[2] + e[7] * reach;
        q[6] = e[3] + e[8] * reach; q[7] = e[4] + TD5_TG_VERGE_LIFT - drop;
        q[8] = e[5] + e[9] * reach;
        q[9] = e[3];               q[10] = e[4] + TD5_TG_VERGE_LIFT;
        q[11] = e[5];
        t[0] = 0.0; t[1] = (double)h->si;
        t[2] = u_r; t[3] = (double)h->si;
        t[4] = u_r; t[5] = (double)h->si + 1.0;
        t[6] = 0.0; t[7] = (double)h->si + 1.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
        if (*h->nmesh >= h->maxmesh) return 1;
        seg_nq = n / 4;
        tg_acct(TG_ACCT_PARK, h->si);
        h->moff[(*h->nmesh)++] = h->blk->len;
        if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                &seg_page, &seg_nq, 1))
            return 0;

        /* Hedge border. [R5 CROSS item 4] The user reported "a green texture
         * around 1m height" that "looks out of place" near crossings: it is this
         * hedge, which stood ONE pavement-width (sw ~= 350 raw) off the kerb and
         * so read as a bright green ~1m WALL right at the roadside wherever a
         * park gap fell in the city (confirmed by framedump at span 92, page
         * R3_BLOCK+1). A real park shows open lawn to the kerb with its boundary
         * hedge at the BACK, so the hedge is set back onto the far part of the
         * lawn (hset) and stood on the sloped lawn surface there (verge lift less
         * the ground-skirt drop at that distance), turning "green wall at the
         * road" into "hedge at the back of the park". Still CULL_NONE so one
         * plane reads both sides. Knob for a single-variable A/B. */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_PARK_HEDGE")) {
            const double hset  = sw + (reach - sw) * 0.75;
            const double hdrop = TD5_TG_GROUND_DROP * hset / TD5_TG_GROUND_WIDTH;
            const double hy    = TD5_TG_VERGE_LIFT - hdrop;
            n = 0; seg_page = TD5_TG_PAGE_R3_BLOCK + 1;   /* park hedge */
            q[0] = e[0] + e[6] * hset;  q[1]  = e[1] + hy;
            q[2] = e[2] + e[7] * hset;
            q[3] = e[3] + e[8] * hset;  q[4]  = e[4] + hy;
            q[5] = e[5] + e[9] * hset;
            q[6] = e[3] + e[8] * hset;  q[7]  = e[4] + hy + TD5_TG_HEDGE_H;
            q[8] = e[5] + e[9] * hset;
            q[9] = e[0] + e[6] * hset;  q[10] = e[1] + hy + TD5_TG_HEDGE_H;
            q[11] = e[2] + e[7] * hset;
            t[0] = 0.0; t[1] = 1.0; t[2] = 1.0; t[3] = 1.0;
            t[4] = 1.0; t[5] = 0.0; t[6] = 0.0; t[7] = 0.0;
            tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
            if (*h->nmesh >= h->maxmesh) return 1;
            seg_nq = n / 4;
            tg_acct(TG_ACCT_PARK, h->si);
            h->moff[(*h->nmesh)++] = h->blk->len;
            if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                    &seg_page, &seg_nq, 1))
                return 0;
        }

        /* One house on ~1 park span in 4, set back in the middle of the lawn. */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_PARK_HOUSES")) {
            const unsigned int hh = ((unsigned)h->si * 2654435761u
                                     + (unsigned)s * 40503u) * 0x9E3779B9u;
            if ((hh >> 30) == 0u) {
                const double set = sw + (reach - sw) * 0.45;
                if (!tg_block_emit_house(h, e, set)) return 0;
            }
        }
    }
    return 1;
}

/* Group BLOCK dispatcher (feedback R3 items 3-6). Wired into the scenery loop
 * next to tg_emit_fb_city; keeps all BLOCK-area emitters out of another area's
 * dispatcher. */
static int tg_emit_fb_block(const TG_FBHook *h)
{
    if (!tg_city_span_paved(h)) return 1;      /* only where the city is */
    if (!tg_block_emit_intersection(h)) return 0;
    if (!tg_block_emit_park(h)) return 0;
    return 1;
}

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

/* [R5 CROSS item 3] "there's still elevated sidewalk on street crossings; those
 * should wrap around the road." R4's item 2 read "raised sidewalk break" as a
 * pedestrian THRESHOLD and stepped the built kerb UP TD5_TG_XKERB_RISE across
 * the crossing (tg_cross_emit_kerb_break). That is the elevated sidewalk the
 * user is now pointing at -- a lip that steps ALONG the kerb rather than a
 * pavement that turns the corner. The wrap the user wants already exists:
 * tg_block_emit_intersection stands a pavement + railing arm at each of the
 * gap's two along-road corners, turning off the main kerb to run down the side
 * street. So the raised break is removed outright (byte attribution: 108 meshes
 * / 23 KB) and the crossing kerb is left flush, with the corner arms doing the
 * wrap. No knob to restore it: a raised threshold at a crossing was the wrong
 * reading of the original request. */

/* item 10 -- buildings LINING a through side street, so it reads as going
 * further with more buildings on its sides. A first cut placed extra rows
 * straight out along the kerb normal (like the back rows, only deeper); a
 * single-variable A/B showed they emitted but were invisible from the racing
 * line -- too far out and occluded by the front frontage. Buildings that don't
 * show are not buildings the user asked for.
 *
 * These instead stand at the TWO along-road edges of the gap (e[0] near, e[3]
 * far) and run OUTWARD down the street, one wall each, facing in -- the left
 * and right frontages of the side street, seen straight down it through the
 * gap. They only ever extend PERPENDICULAR to the main road (down the side
 * street), so nothing here can reach the drivable corridor. The wall runs the
 * cross-street's own reach, a couple of storeys taller than the front row so
 * the canyon has depth.
 *
 * [R5 CROSS item 2] "building facades on every 'lane' of road spawned." A side
 * street is a GAP that is several spans wide, and the first cut emitted BOTH
 * along-road edges on EVERY span of that gap, so a four-span gap grew eight
 * walls -- a comb of parallel frontages marching down the street, one per span
 * boundary, exactly the "facade on every lane" report (byte attribution: 588
 * walls across 274 spans, >2 per gap-span, where a side street wants only its
 * two flanking frontages). The two frontages of a side street sit at the gap's
 * two along-MAIN-road extremes: the NEAR edge of the gap's first span and the
 * FAR edge of its last span. So each edge is now gated to its boundary corner
 * (si-1 built for the near frontage, si+1 built for the far), the same corner
 * test tg_block_emit_intersection uses -- two walls per gap side, no comb. */
static int tg_cross_emit_sidewalls(const TG_FBHook *h)
{
    double px[TD5_TG_FACADE_MAXQUAD * 4], py[TD5_TG_FACADE_MAXQUAD * 4];
    double pz[TD5_TG_FACADE_MAXQUAD * 4], uu[TD5_TG_FACADE_MAXQUAD * 4];
    double vv[TD5_TG_FACADE_MAXQUAD * 4];
    const TG_Biome *b = h->b;
    const double sw = tg_city_sidewalk_w(b);
    const double reach = tg_city_crossst_reach(b, sw);
    int s;

    if (!(sw > 0.0)) return 1;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_CROSS_SIDEWALLS")) return 1;
    if (tg_span_in_bridge_run(h->si)) return 1;
    if (td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE") &&   /* item 15 */
        tg_span_near_bridge(h->si, TD5_TG_XBRIDGE_CLEAR)) return 1;
    if (tg_branches_enabled() && tg_span_in_fork_clear(h->si)) return 1;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        double e[10], ang, ox, oz;
        int edge, near_corner, far_corner;
        if (tg_facade_built(h->si, s)) continue;      /* only a through street */
        if (tg_block_is_park(h->si, s)) continue;     /* a park has no street */
        if (tg_side_blocked(h->si, sg)) continue;
        /* Emit a frontage only at the gap's own boundary spans, so one wide gap
         * grows exactly its two flanking frontages, not one per span. */
        near_corner = tg_facade_built(h->si - 1, s);  /* gap starts here */
        far_corner  = tg_facade_built(h->si + 1, s);  /* gap ends here   */
        if (!near_corner && !far_corner) continue;    /* gap interior: no wall */
        tg_city_edge_frame(h->nl, h->si, sg, e);
        ang = tg_block_arm_skew(h->si, s);
        tg_block_rot2(e[6], e[7], ang, &ox, &oz);     /* outward down the street */

        /* One wall on each along-road edge of the gap: edge 0 = e[0] (near),
         * edge 1 = e[3] (far). Each runs outward `reach`, rising H. The near
         * frontage belongs to the gap-start span, the far to the gap-end span. */
        for (edge = 0; edge < 2; edge++) {
            /* near frontage only at the gap start, far only at the gap end. */
            if (edge == 0 && !near_corner) continue;
            if (edge == 1 && !far_corner)  continue;
            const unsigned int rh = ((unsigned)h->si * 2654435761u
                                     + (unsigned)s * 40503u
                                     + (unsigned)edge * 21841u) * 2246822519u;
            /* [R6 CROSS item 1] The corner building's cap (tg_side_geom
             * cap_near/cap_far) already returns a wall tg_facade_depth(b) down
             * this side street, height-matched to the block. The old frontage
             * started at the road edge and ran the full reach, standing ON that
             * cap for its first stretch -- the "double geometry on the buildings
             * on the side". Start it where the cap ends and line only the street
             * BEYOND the corner block, so cap + frontage read as one building. */
            /* TD5RE_AUTOTRACK_XWALL=0 reverts both halves (base offset + block
             * height) to the pre-R6 frontage for a single-variable A/B. */
            const int xwall = td5_env_flag_on("TD5RE_AUTOTRACK_XWALL");
            const double capd = tg_city_sidewalk_w(b) + tg_facade_depth(b);
            double off = (reach - capd < (double)b->cell_w * 0.5) ? 0.0 : capd;
            double flen;
            if (!xwall) off = 0.0;
            flen = reach - off;
            const double cx = (edge ? e[3] : e[0]) + ox * off;
            const double cz = (edge ? e[5] : e[2]) + oz * off;
            const double cdrop = TD5_TG_GROUND_DROP * off / TD5_TG_GROUND_WIDTH;
            const double by = (edge ? e[4] : e[1]) + TD5_TG_KERB_H - cdrop;
            const double drop = TD5_TG_GROUND_DROP * flen / TD5_TG_GROUND_WIDTH;
            /* [R6 CROSS item 19] Height comes from the adjacent BUILT block, not
             * an independent roll, so a crossing-street frontage agrees with the
             * block it fronts (was b->floors_min+1+rand, which towered over the
             * 1-floor INDUSTRIAL sheds -- "make crossing-street buildings the same
             * height"). rh still keys the page class only. */
            const int nbi = edge ? (h->si + 1) : (h->si - 1);
            int rows = xwall ? tg_facade_floors(nbi, s, b)
                             : (int)(b->floors_min + 1 + (rh >> 9) % 4u);
            int cols, page, seg_nq, n = 0;
            double H;
            if (rows <= 0) rows = b->floors_min + 1;
            H = (double)rows * tg_facade_floor_h(b);
            cols = tg_facade_cols_for(flen, (double)b->cell_w, 5);
            /* base -> outward*flen, sinking with the skirt, rising H. */
            tg_facade_push_grid(cx, by, cz, ox * flen, -drop, oz * flen,
                                0.0, H, 0.0, cols, rows, 0, rows,
                                px, py, pz, uu, vv, &n);
            if (n <= 0) continue;
            if (*h->nmesh >= h->maxmesh) return 1;
            page   = tg_facade_page_class(rh, rows);
            seg_nq = n / 4;
            h->moff[(*h->nmesh)++] = h->blk->len;
            if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                    &page, &seg_nq, 1))
                return 0;
            tg_acct_n(TG_ACCT_CROSSFURN, h->si, 1);
        }
    }
    return 1;
}

/* item 14 -- is span si a short built SLIVER between two crossings on a curve?
 * A run of side streets separated by a one- or two-span wall on a bend reads as
 * a stutter of separate crosswalks; joining them gives one continuous junction.
 * True when this side is BUILT here but has an open (non-park) side street both
 * within TD5_TG_XJOIN_WIN before AND after, and the centreline actually turns
 * across the sliver (a straight run of side streets is left as separate ones). */
static int tg_cross_join_side(const TG_NodeList *nl, int si, int s)
{
    int k, gb = -1, ga = -1;
    double cross;

    if (!tg_facade_built(si, s)) return 0;            /* must be the wall sliver */
    /* Nearest through-street mouth on this side within the window, each way. */
    for (k = 1; k <= TD5_TG_XJOIN_WIN; k++)
        if (si - k >= 0 && !tg_facade_built(si - k, s)
            && !tg_block_is_park(si - k, s)) { gb = si - k; break; }
    for (k = 1; k <= TD5_TG_XJOIN_WIN; k++)
        if (si + k < nl->count && !tg_facade_built(si + k, s)
            && !tg_block_is_park(si + k, s)) { ga = si + k; break; }
    if (gb < 0 || ga < 0) return 0;
    /* Turn measured across the two flanking mouths (a wider, more reliable
     * baseline than the immediate neighbours), so only a genuine bend joins. */
    cross = nl->v[gb].tx * nl->v[ga].tz - nl->v[gb].tz * nl->v[ga].tx;
    return (cross < 0.0 ? -cross : cross) >= TD5_TG_XJOIN_CURVE;
}

/* item 14 -- paint the zebra across a joining sliver span, so two nearby
 * crossings on a curve read as one. Road-surface decal only (PAGE_CROSSING,
 * lifted like tg_city_emit_crossing), no wall or corridor change. */
static int tg_cross_emit_join_zebra(const TG_FBHook *h)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    double l0x, l0y, l0z, r0x, r0y, r0z, l1x, l1y, l1z, r1x, r1y, r1z;
    const double f0 = 0.22, f1 = 0.62, L = (double)h->lanes;
    int seg_page = TD5_TG_PAGE_CROSSING, seg_nq = 1, s, joined = 0, n = 0;

    /* [R7 CROSS item 14] DEFAULT OFF (was default on). The sliver-fill was the
     * R4/R5 answer to "crossings on a curve" -- merge two near ones into one
     * continuous junction. On the tight bend past fork 2 (seed 99991 span 635)
     * it merged a whole RUN of bunched mouths into one radial fan of zebra, the
     * exact "many crossings all together" the user now reports. Min-spacing
     * thinning (tg_city_crossing_here) is the R7 replacement, so the join is
     * retired; TD5RE_AUTOTRACK_CROSS_JOIN=1 restores the legacy behaviour. */
    if (!td5_env_flag_off("TD5RE_AUTOTRACK_CROSS_JOIN")) return 1;
    if (tg_branches_enabled() && tg_span_in_fork_clear(h->si)) return 1;
    if (!tg_span_surface_is_tarmac(h->si)) return 1;   /* [R5 CROSS item 12] */
    for (s = 0; s < 2; s++)
        if (tg_cross_join_side(h->nl, h->si, s)) joined = 1;
    if (!joined) return 1;

    tg_road_edge(h->nl, h->si, f0, 0.0, 1.0, &l0x, &l0y, &l0z, &r0x, &r0y, &r0z);
    tg_road_edge(h->nl, h->si, f1, 0.0, 1.0, &l1x, &l1y, &l1z, &r1x, &r1y, &r1z);
    px[n]=r0x; py[n]=r0y+TD5_TG_CROSS_LIFT; pz[n]=r0z; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=l0x; py[n]=l0y+TD5_TG_CROSS_LIFT; pz[n]=l0z; uu[n]=L;   vv[n]=0.0; n++;
    px[n]=l1x; py[n]=l1y+TD5_TG_CROSS_LIFT; pz[n]=l1z; uu[n]=L;   vv[n]=1.0; n++;
    px[n]=r1x; py[n]=r1y+TD5_TG_CROSS_LIFT; pz[n]=r1z; uu[n]=0.0; vv[n]=1.0; n++;

    if (*h->nmesh >= h->maxmesh) return 1;
    tg_acct_n(TG_ACCT_CROSSFURN, h->si, 1);
    h->moff[(*h->nmesh)++] = h->blk->len;
    {   /* [R8 GUARD] road-surface paint: DECAL, same rule as the crossing zebra. */
        const size_t d0 = h->blk->len;
        int r = tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                   &seg_page, &seg_nq, 1);
        tg_guard_mark(d0, h->blk->len, TG_GK_DECAL, h->si);
        return r;
    }
}

/* Group CROSS dispatcher (feedback R4 items 10, 14; R5 items 2, 3, 4, 12).
 * Wired into the scenery loop next to tg_emit_fb_block; city spans only.
 * R5 item 3 removed the raised kerb break that used to run here. */
static int tg_emit_fb_cross(const TG_FBHook *h)
{
    if (!tg_city_span_paved(h)) return 1;      /* only where the city is */
    if (!tg_cross_emit_sidewalls(h)) return 0;
    if (!tg_cross_emit_join_zebra(h)) return 0;
    return 1;
}

/* Group A -- city: sidewalks, kerb fences, crossings, deeper building rows. */
static int tg_emit_fb_city(const TG_FBHook *h)
{
    const int paved = tg_city_span_paved(h);
    /* [R7 item 7] sw from the SAME hardened city edge tg_city_span_paved uses, so
     * the slab, its kerb height and the railing all agree on where the city is. */
    const double sw = tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(h->si)]);

    if (paved && td5_env_flag_on("TD5RE_AUTOTRACK_SIDEWALKS")) {
        if (!tg_city_emit_sidewalk(h, sw)) return 0;
        if (!tg_city_emit_fence(h, sw)) return 0;
    }
    /* Outside the city the same margin is a texture band, not a slab. Bridge
     * decks are excluded exactly as the pavement is -- there is no ground beside
     * a deck for a band to lie on. */
    if (!paved && !tg_span_in_bridge_run(h->si) &&
        tg_verge_band_w(h->b) > 0.0) {
        if (!tg_city_emit_verge_band(h, tg_verge_band_w(h->b))) return 0;
    }
    if (paved && tg_city_crossing_here(h->si) &&
        td5_env_flag_on("TD5RE_AUTOTRACK_CROSSINGS")) {
        if (!tg_city_emit_crossing(h)) return 0;
    }
    /* The side street itself, on every span of the gap (the zebra above is only
     * on its first span). Both are gated on the pavement, since a gap in a
     * biome with no frontage is just open country. */
    if (paved && !tg_span_in_bridge_run(h->si) &&
        td5_env_flag_on("TD5RE_AUTOTRACK_CROSS_STREETS")) {
        if (!tg_city_emit_crossstreet(h, sw)) return 0;
    }
    /* Lamps follow the biome's prop_lamp flag (city/industrial/coast), on the
     * same 1-in-7 beat the prop layer used, so the spacing is unchanged -- only
     * the fixture under the glow is new. NIGHT ONLY (item 11): a lit lamp head
     * over a midday road is what gives the generated scenery away. */
    if (h->b->prop_lamp && (h->si % 7) == 0 && !tg_span_in_bridge_run(h->si) &&
        td5_trackgen_is_night() &&
        td5_env_flag_on("TD5RE_AUTOTRACK_LAMP_POSTS")) {
        if (!tg_city_emit_lamp(h, sw)) return 0;
    }
    if (paved && !tg_span_in_bridge_run(h->si) &&
        td5_env_flag_on("TD5RE_AUTOTRACK_BACKROWS")) {
        if (!tg_city_emit_backrows(h, sw)) return 0;
    }
    /* [R4 item 6] Fill the bare flank a fork clears on the right with background
     * park/buildings set beyond the corridor. Runs on paved (city) fork spans
     * only; a no-op everywhere else. */
    if (paved) {
        if (!tg_city_emit_forkback(h)) return 0;
    }
    return 1;
}
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
 *   - NO SEAM: u runs 0..1 across a span but ALTERNATES direction per span, so
 *     at every shared edge the same texel column meets itself. A plain repeat
 *     would butt u=1 against u=0, which is only seamless if the page tiles
 *     horizontally, and a noise-built page does not.
 * The band base is sunk below road level so it sits IN the ground skirt rather
 * than floating on it. */
#define TD5_TG_TREELINE_SINK   600.0   /* base below road level, raw */

/* Band height for biome b, 0 = no backdrop. A forest wall stands above the
 * 5400..7200-raw billboards in front of it; alpine conifers read taller; open
 * FIELDS get a low far hedgerow line instead of a wall, which is what keeps the
 * "open horizon" the biome comment asks for. City/industrial/coast/oriental get
 * none: buildings, the sea and manicured planting close those off already. */
static double tg_treeline_height(const TG_Biome *b)
{
    if (!strcmp(b->name, "FOREST")) return 12000.0;
    if (!strcmp(b->name, "ALPINE")) return 14000.0;
    if (!strcmp(b->name, "FIELDS")) return  7000.0;
    return 0.0;
}

/* Lateral setback of the band from the road EDGE. Trees sit at 800..3200 plus
 * half their own width, so the band has to be past that or it would hide them
 * instead of backing them. FIELDS puts its hedgerow line further out again, at
 * the far side of the open ground. */
static double tg_treeline_back(const TG_Biome *b)
{
    return (!strcmp(b->name, "FIELDS")) ? 22000.0 : 11000.0;
}

static int tg_emit_fb_flora(const TG_FBHook *h)
{
    const TG_NodeList *nl = h->nl;
    const int si = h->si;
    double band, back;
    int s;

    /* Default ON (2026-08-26); TD5RE_AUTOTRACK_TREELINE=0 disables the band. */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_TREELINE")) return 1;
    if (si <= TD5_TG_GRID_SPAN) return 1;        /* keep the grid area clear */
    if (si + 1 >= nl->count) return 1;
    if (tg_span_in_bridge_run(si)) return 1;     /* see the river from the deck */
    band = tg_treeline_height(h->b);
    if (!(band > 0.0)) return 1;
    back = tg_treeline_back(h->b);

    /* ONE MESH PER SIDE, not one for both: a mesh spanning both verges has a
     * bounding sphere wider than the band is far away, so the culler could
     * never reject it. Two tight spheres cull properly. */
    for (s = 0; s < 2; s++) {
        double px[4], py[4], pz[4], uu[4], vv[4];
        const double side = s ? 1.0 : -1.0;
        const TG_Node *n0 = &nl->v[si];
        const TG_Node *n1 = &nl->v[si + 1];
        const double lx0 = n0->tz * side, lz0 = -n0->tx * side;
        const double lx1 = n1->tz * side, lz1 = -n1->tx * side;
        /* Clear of any branch carriageway, same rule as the trees. */
        const double d = tg_flora_gap_clear(nl, si, side, back);
        const double e0 = n0->width * 0.5 + d, e1 = n1->width * 0.5 + d;
        const double bx = n0->x + lx0 * e0, bz = n0->z + lz0 * e0;
        const double fx = n1->x + lx1 * e1, fz = n1->z + lz1 * e1;
        const double by = n0->y - TD5_TG_TREELINE_SINK;
        const double fy = n1->y - TD5_TG_TREELINE_SINK;
        /* u alternates per span -- see the seam note above. */
        const double u0 = ((si & 1) == 0) ? 0.0 : 1.0;
        const double u1 = 1.0 - u0;
        int seg_page = TD5_TG_PAGE_TREELINE, seg_nq = 1;

        if (*h->nmesh + 1 >= h->maxmesh) return 1;   /* out of slots this entry */
        /* quad loop: near-bottom, far-bottom, far-top, near-top; v=1 at the
         * base, matching the page convention that row 0 is the TOP. */
        px[0] = bx; py[0] = by;        pz[0] = bz; uu[0] = u0; vv[0] = 1.0;
        px[1] = fx; py[1] = fy;        pz[1] = fz; uu[1] = u1; vv[1] = 1.0;
        px[2] = fx; py[2] = fy + band; pz[2] = fz; uu[2] = u1; vv[2] = 0.0;
        px[3] = bx; py[3] = by + band; pz[3] = bz; uu[3] = u0; vv[3] = 0.0;

        h->moff[*h->nmesh] = h->blk->len;
        if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, 4,
                                &seg_page, &seg_nq, 1)) return 0;
        (*h->nmesh)++;
        tg_acct(TG_ACCT_TREE, si);   /* tree-line band, one per side */
    }
    return 1;
}

/* [R5 item 18] Tall park trees. The user asked a curvy park to justify itself
 * with "a lot of trees ... trees that take up to the ceiling like the sections
 * at the end of Moscow or the middle of Australia", and to "expand the library
 * of available resources". The 10-page borrowed tree set took nothing from
 * Moscow (level023), whose texture set is unusually rich in foliage; this plants
 * a TALL Moscow canopy billboard set well BACK behind each tree biome's own
 * verge planting, so the park (and every billboard-tree biome) gains a high
 * canopy backdrop rising above the near trees rather than a flat hedge line.
 * Real-texture only -- the pages are Moscow art -- and gated for an A/B via
 * TD5RE_R5_FLORA_PARKTREES (default ON). One billboard per qualifying span,
 * accounted under the FLORA area. */
static int tg_emit_fb_park_trees(const TG_FBHook *h)
{
    const TG_NodeList *nl = h->nl;
    const int si = h->si;
    const TG_Biome *b = h->b;
    unsigned int hh;
    double side, gap, tw, th, jit, cx, cz;
    int v, page;

    if (!td5_env_flag_on("TD5RE_R5_FLORA_PARKTREES")) return 1;
    if (!tg_real_textures_enabled())  return 1;   /* pages are Moscow art */
    if (k_r5_flora_tree_count <= 0)   return 1;
    if (!b->billboard || b->tree_n <= 0) return 1;/* tree biomes only, not facades */
    if (si <= TD5_TG_GRID_SPAN)       return 1;   /* keep the grid area clear */
    if (si + 1 >= nl->count)          return 1;
    if (tg_span_in_bridge_run(si))    return 1;   /* deck is clear */
    if (*h->nmesh + 1 >= h->maxmesh)  return 1;

    /* Sparser than the near trees: a grove on roughly one span in three, so the
     * tall canopy reads as clumps rather than a continuous second wall. */
    hh = (unsigned)si * 2246822519u + 0x2545F491u;
    if ((int)(hh >> 29) > (b->density >> 1)) return 1;

    side = ((hh >> 4) & 1) ? 1.0 : -1.0;
    if (tg_side_blocked(si, side)) return 1;

    /* [R6 items 7/18] Default ON; TD5RE_R6_FLORA=0 restores the R5 park tree
     * (grove-wall page in the upright rotation + 4200-wide billboards) for A/B. */
    if (td5_env_flag_on("TD5RE_R6_FLORA")) {
        v = tg_flora_upright_index(hh >> 13);    /* skip the grove-backdrop wall */
        if (v < 0) return 1;
    } else {
        v = (int)((hh >> 13) % (unsigned)k_r5_flora_tree_count);
    }
    page = tg_flora_tree_slot(v);

    /* Tall: the near verge trees top out around 7200 raw (the palm); these reach
     * ~10000 raw (~40 world units) so they stand above that as a canopy. */
    jit = 1.40 + (double)((hh >> 9) % 61) * 0.01;   /* 1.40 .. 2.00 */
    /* WIDTH [R6 items 7/18]: was 4200*jit (up to 8400). A single flat billboard
     * that wide reads as a WALL/treeline beside the road and looms over a curved
     * approach ("bigger than the area ... spill onto the road"). A tree is
     * slender -- the near-verge species measure ~2900..4700 wide and read
     * correctly -- so narrow to 2600*jit (3640..5200) while KEEPING the extra
     * height the "ceiling-tall Moscow trees" request wanted. */
    tw  = (td5_env_flag_on("TD5RE_R6_FLORA") ? 2600.0 : 4200.0) * jit;
    th  = 5200.0 * jit;
    gap = 2600.0 + (double)((hh >> 5) % 3200);      /* set BACK, behind the verge */
    gap = tg_flora_gap_clear(nl, si, side, gap);    /* never on a branch */

    {   /* [R7 item 18] on the ground, never on water/coastline */
        double base_y;
        if (!tg_flora_plant(nl, si, b, side, gap, tw, &cx, &cz, &base_y))
            return 1;

        tg_flora_diag(nl, si, "park", page, side, tw, th, cx, cz);
        h->moff[*h->nmesh] = h->blk->len;
        if (!tg_emit_billboard_mesh(h->blk, cx, base_y, cz, tw * 0.5, th, page, 1))
            return 0;
        (*h->nmesh)++;
        tg_acct(TG_ACCT_R5_FLORA, si);
        return 1;
    }
}
/* How many spans either side of a portal carry mountain massing. Beyond this
 * the camera is inside the bore and the mass is behind the lining, so it is
 * invisible geometry -- 5 spans (~4000 units) is what a driver actually sees
 * from outside the mouth. */
#define TD5_TG_TUNNEL_MASS_SPANS 5

/* Spans from si to the nearest END of its tunnel run, capped. tg_span_in_tunnel
 * is a pure hash of si, so this is a bounded walk of at most cap+1 steps, not a
 * search over the track. Negative si is safe: the function early-outs on
 * si <= TD5_TG_GRID_SPAN + 40 before touching anything. */
static int tg_tunnel_edge_dist(int si, int cap)
{
    int d;
    for (d = 0; d <= cap; d++)
        if (!tg_span_in_tunnel(si - d) || !tg_span_in_tunnel(si + d))
            return d;
    return cap + 1;
}

/* Group C -- tunnels: portal surrounds, mountain massing, width for branches.
 * Called INSIDE the tunnel branch, where no buildings/props are emitted.
 *
 * ROOT CAUSE this addresses: tg_emit_tunnel emits an enclosure and nothing
 * else, so a tunnel in open country is a concrete box sitting ON the landscape
 * with sky above and behind it -- the road does not go THROUGH anything, so the
 * mouth reads as the end of a shed. Fix by putting mass over and beside the
 * bore: a CROWN slab stacked on the roof and a SHOULDER each side, plus, at the
 * portal span itself, two BUTTRESSES hugging the mouth so the opening reads as
 * cut into rock. Because this hook only runs on tunnel spans, the mountain
 * begins exactly at the mouth -- which is what you want: from outside you see a
 * rock face with a hole in it.
 *
 * Weighted toward the PORTALS, and skipped past TD5_TG_TUNNEL_MASS_SPANS, for
 * the reason above: deep inside the run the mass is occluded by the lining, so
 * emitting it there would only spend mesh budget.
 *
 * Pages: TD5_TG_PAGE_HILL, read-only (Group D fills it) -- a hillside flank is
 * exactly what this is. Vertex colour darkens the mass slightly so it does not
 * read as the same material as the lining. */
static int tg_emit_fb_tunnel(const TG_FBHook *h)
{
    const TG_Node *n = &h->nl->v[h->si];
    const double lx = n->tz, lz = -n->tx;
    const double roof_top = n->y + TD5_TG_TUNNEL_HEIGHT + 400.0;
    const unsigned int rock  = 0xFFC0C8C0u;   /* lit rock/turf flank */
    const unsigned int shade = 0xFF98A098u;   /* the cut face at the mouth */
    double bore_half, bore_shift, side_x, cx, cz, vis, crown_h, flank_hx;
    int ed, s;

    /* Default ON (a fix); TD5RE_AUTOTRACK_TUNNEL_MOUNTAIN=0 to disable. */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_TUNNEL_MOUNTAIN")) return 1;

    ed = tg_tunnel_edge_dist(h->si, TD5_TG_TUNNEL_MASS_SPANS);
    if (ed > TD5_TG_TUNNEL_MASS_SPANS) return 1;      /* buried out of sight */
    if (*h->nmesh + 6 > h->maxmesh) return 1;         /* budget, not an error */

    /* 1.0 at the portal span, tapering inward -- the hill thins as it goes
     * behind the lining, which is also how a real cutting looks from outside. */
    vis = (double)(TD5_TG_TUNNEL_MASS_SPANS + 1 - ed) /
          (double)TD5_TG_TUNNEL_MASS_SPANS;

    tg_tunnel_bore(h->nl, h->si, &bore_half, &bore_shift);
    side_x = bore_half + TD5_TG_TUNNEL_WALL_T;
    cx = n->x + lx * bore_shift;
    cz = n->z + lz * bore_shift;

    crown_h  = 700.0 + 1900.0 * vis;
    flank_hx = 900.0 + 2200.0 * vis;

    /* CROWN: mass stacked directly on the roof slab, overhanging it a little so
     * no sliver of sky shows between hill and tunnel. */
    h->moff[(*h->nmesh)++] = h->blk->len;
    if (!tg_emit_box_mesh(h->blk, cx, roof_top + crown_h * 0.5, cz,
                          side_x + 900.0, crown_h * 0.5, 780.0,
                          n->tx, n->tz, TD5_TG_PAGE_HILL, 3000.0, rock))
        return 0;

    /* SHOULDERS: one each side, outboard of the roof overhang so they never
     * intrude into the bore, sunk well below the road so they meet whatever
     * terrain the ground pass laid down instead of floating over it. */
    for (s = 0; s < 2; s++) {
        const double sgn = s ? 1.0 : -1.0;
        const double off = side_x + 900.0 + flank_hx;
        const double top = roof_top - 600.0;
        const double bot = n->y - 2500.0;
        h->moff[(*h->nmesh)++] = h->blk->len;
        if (!tg_emit_box_mesh(h->blk, cx + lx * off * sgn, (top + bot) * 0.5,
                              cz + lz * off * sgn,
                              flank_hx, (top - bot) * 0.5, 780.0,
                              n->tx, n->tz, TD5_TG_PAGE_HILL, 3000.0, rock))
            return 0;
    }

    /* BUTTRESSES: only on the portal span itself. Deeper along the road than a
     * span slab (1400 vs 780) so the mouth sits in a recess rather than flush
     * with the hillside, which is what makes it read as a bore rather than a
     * hole painted on a wall. */
    if (ed <= 1) {
        for (s = 0; s < 2; s++) {
            const double sgn = s ? 1.0 : -1.0;
            const double off = side_x + 800.0;
            if (*h->nmesh + 1 > h->maxmesh) break;
            h->moff[(*h->nmesh)++] = h->blk->len;
            if (!tg_emit_box_mesh(h->blk, cx + lx * off * sgn, n->y + 900.0,
                                  cz + lz * off * sgn,
                                  800.0, 2400.0, 1400.0,
                                  n->tx, n->tz, TD5_TG_PAGE_HILL, 2400.0,
                                  shade))
                return 0;
        }
    }
    return 1;
}
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

/* THE CURE for the ceiling above, replacing the reach cut that only hid it.
 *
 * The band was flat: all four of its rings sat at the emitting span's own road
 * height, so a band on high ground was a slab of high ground hanging over
 * whatever low ground it reached. Height is a LOCAL fact but the band's extent
 * is not, which is the whole defect -- and the same shape as the 2026-08-24
 * elevated-bridge gate, one axis over.
 *
 * So make it a SLOPE instead of a slab. Ring 0 still meets the skirt at the
 * local road height, because that seam has to stay watertight; from there the
 * band descends to the track's GLOBAL minimum (tg_track_min_y) and stays there
 * for the outer two rings. Two consequences, and both are the point:
 *   - beyond ring 2 every band on the track is at the SAME height, so however
 *     many of them overlap out there they are coplanar, not stacked;
 *   - nothing above the global floor survives past 45% of the reach, so the
 *     span of track a band can hang over shrinks from the full reach to that.
 * On the measured seed 1234567 (relief 10662, reach 30000) that is the
 * difference between a 6400-unit ceiling and none.
 *
 * TD5RE_AUTOTRACK_TERRAIN_SINK=0 restores the flat slab. */
static const double k_tg_far_sink[4] = { 0.0, 0.45, 1.0, 1.0 };
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

/* Default ON -- these are fixes. TERRAIN_FAR=0 restores the short skirt only,
 * TERRAIN_HILLS=0 keeps the long plain but drops the distant ridge wall. */
static int tg_terrain_far_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_TERRAIN_FAR");
}

static int tg_terrain_ridge_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_TERRAIN_HILLS");
}

/* [R4 item 5] Two independent far-ridge fixes, each its own single-variable
 * flag (both default ON):
 *  - TREELINE_FIX: half-texel V inset that kills the "black line at the top".
 *    Safe by construction (matches the facade UV-inset precedent).
 *  - CITY_SKYLINE: route the ridge to a blocky building skyline in urban biomes
 *    instead of a forest canopy ("which ones are suitable for cities"). A
 *    judgment change, split out so it can be A/B'd and reverted on its own. */
static int tg_r4_treeline_fix(void)
{
    return td5_env_flag_on("TD5RE_R4_TREELINE_FIX");
}

static int tg_r4_city_skyline(void)
{
    return td5_env_flag_on("TD5RE_R4_CITY_SKYLINE");
}

/* [R5 item 16] Tree-line stretch + "height not following the grass" fix. Default
 * ON; TD5RE_R5_FLORA_TREELINE=0 restores the pre-fix band for an A/B. */
static int tg_r5_treeline_fix(void)
{
    return td5_env_flag_on("TD5RE_R5_FLORA_TREELINE");
}

/* Smooth low-frequency terrain height at a world point. Two summed products of
 * sines at ~42000 and ~17000 world units, which at a 1500-unit span length is a
 * hill every ~28 and ~11 spans -- long enough to read as topology from a car
 * rather than as noise. Result is in [-amp, +amp]. */
static double tg_terrain_hill_y(double x, double z, double amp)
{
    const double s1 = 1.0 / 42000.0, s2 = 1.0 / 17000.0;
    return amp * (0.70 * sin(x * s1 + 1.3) * cos(z * s1 - 0.4)
                + 0.30 * sin(x * s2 - 2.1) * cos(z * s2 + 0.9));
}

/* Does span si emit its group's band? The FIRST non-tunnel span of the group
 * does. The hook is not called on tunnel spans at all, so keying on si % GROUP
 * would silently drop a band wherever a tunnel run happened to cover the
 * group's first span. */
static int tg_far_group_owner(int si)
{
    const int g0 = (si / TD5_TG_FAR_GROUP) * TD5_TG_FAR_GROUP;
    int j;
    for (j = g0; j < si; j++)
        if (!tg_span_in_tunnel(j)) return 0;
    return 1;
}

/* One side's background band: 3 ground quads outward plus the ridge wall. */
static int tg_emit_far_band(const TG_FBHook *h, int is_left)
{
    const TG_NodeList *nl = h->nl;
    const double reach = (double)td5_env_int("TD5RE_AUTOTRACK_TERRAIN_REACH",
                                             TD5_TG_FAR_REACH, 30000, 400000);
    /* Band height envelope: flat at the seam, rolling further out. */
    static const double k_amp[4] = { 0.0, 700.0, 2200.0, 4200.0 };
    const int sink = td5_env_flag_on("TD5RE_AUTOTRACK_TERRAIN_SINK");
    const double floor_y = tg_track_min_y(nl) - TD5_TG_FAR_SINK_AT;
    const int g0 = (h->si / TD5_TG_FAR_GROUP) * TD5_TG_FAR_GROUP;
    int g1 = g0 + TD5_TG_FAR_GROUP - 1;
    double X[2][4], Y[2][4], Z[2][4], D[2][4], U[2], B[2];
    double px[16], py[16], pz[16], uu[16], vv[16];
    int seg_page[2], seg_nq[2];
    int e, j, n = 0, nseg = 1;
    /* [R5 item 16] The height/stretch cure only applies where the ridge is an
     * actual tree line: non-snow, non-urban (snow keeps its flank, urban gets the
     * blocky skyline -- see the seg_page[1] routing below). */
    const int r5fix = tg_r5_treeline_fix();
    const int r5treeline = !tg_biome_is_snow(h->b) && h->b->urbanity < 2;

    if (g1 > nl->count - 2) g1 = nl->count - 2;
    if (g1 < g0) return 1;

    for (e = 0; e < 2; e++) {
        const int se = e ? g1 : g0;
        double lx, ly, lz, rx, ry, rz, ux, uz, len, so, base;
        TG_GroundProf p;

        tg_road_edge(nl, se, e ? 1.0 : 0.0, 0.0, 1.0,
                     &lx, &ly, &lz, &rx, &ry, &rz);
        ux = lx - rx; uz = lz - rz;
        len = sqrt(ux * ux + uz * uz);
        if (len < 1e-6) { ux = 1.0; uz = 0.0; } else { ux /= len; uz /= len; }
        if (!is_left) { ux = -ux; uz = -uz; }

        /* Start where the skirt ended, from the SAME profile the skirt used --
         * including its WATER SIDE. This used to hardcode 0.0 while
         * tg_emit_ground passes the real side, so on a coastal run the band and
         * the skirt disagreed about where the ground was. */
        {
            const double wsd = h->b->water ? tg_water_side(se) : 0.0;
            double drop;

            tg_ground_side(nl, se, is_left, wsd, &p);
            so   = p.d[p.n - 1];
            drop = p.dy[p.n - 1];
            /* Belt and braces on top of the clamp in tg_ground_side: this band
             * runs out to TD5_TG_FAR_REACH (180000), so ANY upward error here is
             * multiplied into a slab across the whole view. Measured on seed
             * 1234567 span 218 before the fix: the seam sat 6350 units ABOVE the
             * road and the player drove under its underside, which read in frame
             * as the sky being replaced by a dark ceiling. The far terrain is
             * never allowed above the road edge. */
            if (drop < 0.0) drop = 0.0;
            base = (is_left ? ly : ry) - drop - TD5_TG_FAR_SINK;
        }
        U[e] = (double)se + (e ? 1.0 : 0.0);
        B[e] = base;

        /* Geometric spacing: each band is roughly twice the depth of the one
         * inside it, so the near ground still has detail while three quads
         * still cover ten times the old reach. */
        D[e][0] = so - TD5_TG_FAR_TUCK;
        D[e][1] = so + (reach - so) * 0.18;
        D[e][2] = so + (reach - so) * 0.45;
        D[e][3] = reach;

        for (j = 0; j < 4; j++) {
            const double ex = (is_left ? lx : rx) + ux * D[e][j];
            const double ez = (is_left ? lz : rz) + uz * D[e][j];
            /* Descend from the seam to the GLOBAL floor -- see k_tg_far_sink.
             * Never the other way: where the emitting span IS the low point of
             * the track the floor is above the seam, and lifting the band onto
             * it would recreate the ceiling this cures. */
            double yb   = sink ? base + (floor_y - base) * k_tg_far_sink[j] : base;
            double ampj = k_amp[j];
            if (yb > base) yb = base;
            /* [R5 item 16] On a tree line, replace the deep floor sink of the two
             * outer points with a shallow, gently-rolling drop below the seam, so
             * the ridge that stands on j==3 follows the near grass instead of
             * dropping into a jagged trough. Kept strictly at or below `base`. */
            if (r5fix && r5treeline && j >= 2) {
                yb    = base - TD5_TG_TREELINE_BASE_DROP * k_tg_far_sink[j];
                ampj *= TD5_TG_TREELINE_AMP_SCALE;
                if (yb > base) yb = base;
            }
            X[e][j] = ex;
            Z[e][j] = ez;
            Y[e][j] = yb + tg_terrain_hill_y(ex, ez, ampj);
            if (r5fix && r5treeline && j >= 2 && Y[e][j] > base) Y[e][j] = base;
        }

        /* [DIAG] Every term that decides how high this band sits, so the large
         * one identifies ITSELF. Three inferred mechanisms for the "ceiling"
         * have already been wrong; this prints the arithmetic instead.
         * TD5RE_AUTOTRACK_FAR_LOG=1. */
        if (td5_env_flag_off("TD5RE_AUTOTRACK_FAR_LOG"))
            TD5_LOG_I(LOG_TAG,
                      "farband si=%d %s e=%d biome=%s | road_y=%.0f edge_y=%.0f "
                      "prof_n=%d prof_dy_last=%.0f base=%.0f floor=%.0f sink=%d "
                      "| Y0=%.0f Y3=%.0f | lift_vs_road=%.0f",
                      h->si, is_left ? "L" : "R", e, h->b->name,
                      nl->v[se].y, (is_left ? ly : ry),
                      p.n, p.dy[p.n - 1], base, floor_y, sink,
                      Y[e][0], Y[e][3], Y[e][0] - nl->v[se].y);
    }

    /* Apron quads, same ring order as the skirt (near-in, near-out, far-out,
     * far-in) so the winding matches geometry that is known to draw. U is the
     * outward distance in span-lengths, matching the skirt's square tiling. */
    for (j = 0; j < 3; j++) {
        px[n]=X[0][j];   py[n]=Y[0][j];   pz[n]=Z[0][j];
        uu[n]=D[0][j]  /(double)TD5_TG_SPAN_LENGTH; vv[n]=U[0]; n++;
        px[n]=X[0][j+1]; py[n]=Y[0][j+1]; pz[n]=Z[0][j+1];
        uu[n]=D[0][j+1]/(double)TD5_TG_SPAN_LENGTH; vv[n]=U[0]; n++;
        px[n]=X[1][j+1]; py[n]=Y[1][j+1]; pz[n]=Z[1][j+1];
        uu[n]=D[1][j+1]/(double)TD5_TG_SPAN_LENGTH; vv[n]=U[1]; n++;
        px[n]=X[1][j];   py[n]=Y[1][j];   pz[n]=Z[1][j];
        uu[n]=D[1][j]  /(double)TD5_TG_SPAN_LENGTH; vv[n]=U[1]; n++;
    }
    seg_page[0] = tg_ground_page_for_span(h->si, h->b);
    seg_nq[0]   = 3;

    if (tg_terrain_ridge_enabled()) {
        /* Ridge: a wall standing on the outermost edge, its top sampled from the
         * same hill function so consecutive groups share a crest height and the
         * skyline is one continuous ridge line. Snow biomes get a white flank
         * for the same reason the skirt does. */
        double t0 = TD5_TG_RIDGE_BASE + tg_terrain_hill_y(X[0][3], Z[0][3], 3600.0);
        double t1 = TD5_TG_RIDGE_BASE + tg_terrain_hill_y(X[1][3], Z[1][3], 3600.0);
        if (t0 < TD5_TG_RIDGE_MIN_UP) t0 = TD5_TG_RIDGE_MIN_UP;
        if (t1 < TD5_TG_RIDGE_MIN_UP) t1 = TD5_TG_RIDGE_MIN_UP;
        /* The wall's height is measured from its own base, and that base is now
         * on the global floor rather than under the emitting span. On a stretch
         * of road high above the floor a purely relative crest therefore falls
         * BELOW the horizon and the skyline opens up. Require it to clear the
         * road it is seen from instead, which is what a ridge does. */
        if (Y[0][3] + t0 < B[0] + TD5_TG_RIDGE_MIN_UP)
            t0 = B[0] + TD5_TG_RIDGE_MIN_UP - Y[0][3];
        if (Y[1][3] + t1 < B[1] + TD5_TG_RIDGE_MIN_UP)
            t1 = B[1] + TD5_TG_RIDGE_MIN_UP - Y[1][3];
        /* [R4 item 5] Half-texel V inset. The band maps v=0 at the crest and
         * v=1 at the base; reaching EXACTLY 0.0/1.0 samples the page's edge texel
         * row, and under the wrapped sampler the top edge (v=0) fetches the
         * bottom row (v=1, the darkest foliage base) -- the reported "black line
         * at the top of the texture". e0..e1 keeps both edges off the border.
         * flag off restores the raw 0/1 for the A/B. */
        const int    tfix = tg_r4_treeline_fix();
        const double vt = tfix ? TD5_TG_FACADE_UV_INSET       : 0.0;
        const double vb = tfix ? 1.0 - TD5_TG_FACADE_UV_INSET : 1.0;
        /* [R5 item 16] Horizontal tiling. The ridge quad spans one far-group, but
         * its two ends are at TD5_TG_FAR_REACH (30000) OUT, so on a curve they fan
         * far wider than the FAR_GROUP span-index delta the old U used (a fixed 4
         * tiles). That stretched each 64-texel canopy tile to thousands of world
         * units -- the reported "trees texture looks very stretched out". Tile by
         * the ridge's ACTUAL world width instead, one tile per span-length, so a
         * tile is the same size on a straight and through a bend. The page wraps
         * and every quad starts at U=0 and ends on an integer tile count, so the
         * join to the next group's quad stays continuous. */
        double u_near = U[0], u_far = U[1];
        if (r5fix && r5treeline) {
            const double w = sqrt((X[1][3]-X[0][3])*(X[1][3]-X[0][3])
                                + (Z[1][3]-Z[0][3])*(Z[1][3]-Z[0][3]));
            double tiles = floor(w / (double)TD5_TG_SPAN_LENGTH + 0.5);
            if (tiles < 1.0) tiles = 1.0;
            u_near = 0.0; u_far = tiles;
        }
        /* near-bottom, far-bottom, far-top, near-top; v = 1 at the base, so the
         * page's top rows (v = 0) land on the crest. */
        px[n]=X[0][3]; py[n]=Y[0][3];      pz[n]=Z[0][3]; uu[n]=u_near; vv[n]=vb; n++;
        px[n]=X[1][3]; py[n]=Y[1][3];      pz[n]=Z[1][3]; uu[n]=u_far;  vv[n]=vb; n++;
        px[n]=X[1][3]; py[n]=Y[1][3] + t1; pz[n]=Z[1][3]; uu[n]=u_far;  vv[n]=vt; n++;
        px[n]=X[0][3]; py[n]=Y[0][3] + t0; pz[n]=Z[0][3]; uu[n]=u_near; vv[n]=vt; n++;
        /* [R3 item 8] This distant ridge IS the "background tree line" the user
         * reported as "grey at the bottom and white at the top ... doesn't seem
         * to be a tree texture there". It was drawn on TD5_TG_PAGE_HILL, whose
         * palette is literally grey rock (idx 0..9) under a white snowline (idx
         * 10..15) -- correct for a snowy peak, wrong everywhere else, and NOT a
         * tree. (The r2 attempt rebuilt the flora TREELINE band instead, which
         * never even emits on a seed with no FOREST/ALPINE/FIELDS biome -- e.g.
         * 99991 -- so it could not have fixed what is on screen.) Draw the ridge
         * on the green alpha-keyed canopy page in every non-snow biome: its
         * ragged keyed top reads as a forested skyline silhouette against the
         * sky, which is exactly the tree line the user expected. Snow/alpine keep
         * the snowy flank. TD5_TG_PAGE_HILL stays the tunnel rock massing page,
         * untouched. */
        /* [R4 item 5] "which ones are suitable for cities": a forest canopy
         * behind a city skyline reads wrong. Snow keeps its white flank; the
         * urban biomes (urbanity >= 2: CITY, INDUSTRIAL, ORIENTAL) get the blocky
         * building-tops skyline page; everything else keeps the tree line. Gated
         * so the A/B toggles ONE thing. */
        if (tg_biome_is_snow(h->b))
            seg_page[1] = tg_ground_page_for_span(h->si, h->b);
        else if (tg_r4_city_skyline() && h->b->urbanity >= 2)
            seg_page[1] = TD5_TG_PAGE_R4_SKYLINE;
        else
            seg_page[1] = TD5_TG_PAGE_TREELINE;
        /* [R4 seam] account the urban-skyline ridge under the FLOW bucket so the
         * inventory reflects the reclassification (renamed in place). */
        if (seg_page[1] == TD5_TG_PAGE_R4_SKYLINE)
            tg_acct(TG_ACCT_R4_FLOW, h->si);
        seg_nq[1]   = 1;
        nseg = 2;
    }

    h->moff[(*h->nmesh)] = h->blk->len;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, seg_page, seg_nq, nseg))
        return 0;
    (*h->nmesh)++;
    /* One band covers the whole far-group, not just its owner span. */
    tg_acct_range(TG_ACCT_FARBAND, g0, g1);
    return 1;
}

/* [R5 item 14] Does the far-group owned by span si sit over a bridge run?
 * The far band is emitted per 4-span group (TD5_TG_FAR_GROUP); a bridge run is
 * 40 spans and 40-aligned, so a whole run maps onto ten complete groups and no
 * group straddles a run boundary. Test both group ends anyway so a future
 * unaligned run still reads correctly. */
static int tg_far_group_over_bridge(int si)
{
    const int g0 = (si / TD5_TG_FAR_GROUP) * TD5_TG_FAR_GROUP;
    const int g1 = g0 + TD5_TG_FAR_GROUP - 1;
    return tg_span_in_bridge_run(g0) || tg_span_in_bridge_run(g1);
}

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
static int tg_far_group_near_tunnel(int si)
{
    const int m = td5_env_int("TD5RE_AUTOTRACK_TUNNEL_FARCLEAR",
                              TD5_TG_TUNNEL_FARCLEAR, 0, 400);
    const int g0 = (si / TD5_TG_FAR_GROUP) * TD5_TG_FAR_GROUP;
    const int g1 = g0 + TD5_TG_FAR_GROUP - 1;
    int s;
    for (s = g0 - m; s <= g1 + m; s++)
        if (s >= 0 && tg_span_in_tunnel(s)) return 1;
    return 0;
}

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
static int tg_far_group_over_fork(int si)
{
    const int g0 = (si / TD5_TG_FAR_GROUP) * TD5_TG_FAR_GROUP;
    const int g1 = g0 + TD5_TG_FAR_GROUP - 1;
    int i;
    if (!tg_branches_enabled()) return 0;
    for (i = 0; i < s_fork_count; i++) {
        const int a = s_forks[i].F - TD5_TG_BRANCH_WIDEN - 2;
        const int b = s_forks[i].R + TD5_TG_FAR_FORK_PAD;
        if (g1 >= a && g0 <= b) return 1;   /* group overlaps the fork + bow */
    }
    return 0;
}

static int tg_emit_fb_terrain(const TG_FBHook *h)
{
    double wsd;
    int s;

    if (!tg_terrain_far_enabled()) return 1;
    if (!tg_far_group_owner(h->si)) return 1;

    /* [R5 item 14] Leave ONLY water on the floor below a bridge. The distant
     * terrain apron + ridge/skyline wall (tg_emit_far_band) is not gated on the
     * crossing, so over every bridge run it draped a grass/concrete apron and a
     * building-tops skyline down into the gorge -- the "still plenty of road,
     * building backgrounds mixed with grass" report. Suppress it over a bridge
     * group: the bridge water plane (BRIDGE_WATER_HALF 32000) already reaches
     * past the far band's own reach (FAR_REACH 30000), so removing the band
     * leaves open water to the horizon with nothing to cover, and the sloped
     * shore is carried by the gorge banks (tg_ground_side gorge branch) and the
     * longitudinal coastline strips (tg_emit_bridge_coast). The suppression
     * shows in the element inventory as a far-bands GAP over each run.
     * TD5RE_AUTOTRACK_BRIDGE_CLEARFAR=0 restores the draped band for an A/B. */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_CLEARFAR") &&
        tg_far_group_over_bridge(h->si))
        return 1;

    /* [R6 TUNNEL item 8a] Same idea for tunnels: the distant ridge of the groups
     * flanking a bore shows through the mouth as "geometry crossing the road
     * inside the tunnel". Suppress the band near a run; the portal mountain
     * massing carries the immediate surround. TD5RE_AUTOTRACK_TUNNEL_CLEARFAR=0
     * restores the band for an A/B. */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_TUNNEL_CLEARFAR") &&
        tg_far_group_near_tunnel(h->si))
        return 1;

    /* Seaward side is the sea's, not the plain's -- the water plane already
     * reaches 50000 out there and a grass band would float over it. */
    wsd = h->b->water ? tg_water_side(h->si) : 0.0;

    for (s = 0; s < 2; s++) {
        const int is_left = s ? 1 : 0;
        if ((wsd > 0.0 && is_left) || (wsd < 0.0 && !is_left)) continue;
        /* [R6 item 4] Skip the RIGHT band over a fork: it lands on the branch,
         * which the forkback backdrop already closes. Left band is untouched. */
        if (!is_left && td5_env_flag_on("TD5RE_AUTOTRACK_FORK_CLEARFAR") &&
            tg_far_group_over_fork(h->si))
            continue;
        if (*h->nmesh + 2 >= h->maxmesh) break;
        if (!tg_emit_far_band(h, is_left)) return 0;
    }
    return 1;
}
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

static int tg_r4_banner_fix(void)
{
    return td5_env_flag_on("TD5RE_R4_BANNER_FIX");   /* default ON */
}

/* [R5 STRUCT item 1] Point the gantry LEGS at their own solid concrete page
 * (TD5_TG_PAGE_R5_LEG) instead of the bleeding 12% window of the shared BANNER
 * page. Default ON; =0 pins the round-4 legs-on-BANNER for a single-variable
 * A/B of the leg texture. */
static int tg_r5_leg_fix(void)
{
    return td5_env_flag_on("TD5RE_R5_STRUCT_LEG");   /* default ON */
}

/* Push one axis-aligned-in-the-road-frame quad into the caller's arrays. */
static void tg_gantry_quad(double *px, double *py, double *pz,
                           double *uu, double *vv, int *n,
                           const double *x, const double *y, const double *z,
                           double u0, double u1, double v0, double v1)
{
    const double us[4] = { u0, u1, u1, u0 };
    const double vs[4] = { v0, v0, v1, v1 };
    int i;
    for (i = 0; i < 4; i++) {
        px[*n] = x[i]; py[*n] = y[i]; pz[*n] = z[i];
        uu[*n] = us[i]; vv[*n] = vs[i];
        (*n)++;
    }
}

/* Gantry across span si. `finish` selects the FINISH artwork over the START
 * artwork. Returns 0 on OOM. */
static int tg_emit_gantry(const TG_NodeList *nl, int si, TG_Buf *blk, int finish)
{
    double lx, ly, lz, rx, ry, rz;     /* road edge at the span's near row */
    double dx, dz, len;                /* unit lateral, left-positive      */
    double tx, tz;                     /* unit along the road              */
    double px[64], py[64], pz[64], uu[64], vv[64];
    double qx[4], qy[4], qz[4];
    double base_y;
    /* One command per page, in vertex order: the frame first, then the two
     * halves of the word (see the panel block). [R5 item 1] a 4th segment when
     * the legs move to their own page (legs page, cap page, then the two word
     * halves). */
    int seg_page[4], seg_nq[4];
    int nseg = 3, cap_n = 0;
    int n = 0, i;
    const int    legfix = tg_r5_leg_fix();
    const double lue     = legfix ? TD5_TG_FACADE_UV_INSET : 0.0;
    /* [R4 item 1] one flag drives both halves of the fix so the A/B is single
     * variable. leg_w/leg_d square the posts up; `out` moves the feet outboard
     * to match; `ins` insets the panel UVs to kill the centre seam. */
    const int    fix   = tg_r4_banner_fix();
    const double leg_w = fix ? TD5_TG_GANTRY_LEG_W2 : TD5_TG_GANTRY_LEG_W;
    const double leg_d = fix ? TD5_TG_GANTRY_LEG_D2 : TD5_TG_GANTRY_THICK;
    const double out   = fix ? TD5_TG_GANTRY_OUT2   : TD5_TG_GANTRY_OUT;
    const double ins   = fix ? TD5_TG_FACADE_UV_INSET : 0.0;

    tg_road_edge(nl, si, 0.0, 0.0, 1.0, &lx, &ly, &lz, &rx, &ry, &rz);
    dx = lx - rx; dz = lz - rz;
    len = sqrt(dx * dx + dz * dz);
    if (len < 1e-6) return 1;          /* degenerate span: nothing to straddle */
    dx /= len; dz /= len;
    /* Along-road unit is the lateral rotated 90 deg (left of travel is
     * (tz,-tx), so travel is (-dz, dx) in the same convention). */
    tx = -dz; tz = dx;
    /* Legs stand on the LOWER of the two edges so neither foot floats on a
     * cambered or graded span. */
    base_y = (ly < ry) ? ly : ry;

    /* --- two uprights, each a 4-quad box (no top/bottom: the panel covers the
     * top and the ground covers the bottom) --- */
    for (i = 0; i < 2; i++) {
        const double ex = i ? rx : lx, ez = i ? rz : lz;
        const double s  = i ? -1.0 : 1.0;              /* outboard direction */
        const double cxx = ex + dx * s * out;
        const double czz = ez + dz * s * out;
        const double y0 = base_y - 40.0;               /* sunk, no gap        */
        const double y1 = base_y + TD5_TG_GANTRY_CLEAR + TD5_TG_GANTRY_PANEL_H;
        int face;
        /* Four faces of the post, each spanned by the lateral or the along-road
         * axis; the page's V runs up the post. */
        for (face = 0; face < 4; face++) {
            /* (a,b) = the in-plane axis, (c) = the fixed offset axis. */
            const double ax = (face < 2) ? dx : tx, az = (face < 2) ? dz : tz;
            const double ox = (face < 2) ? tx : dx, oz = (face < 2) ? tz : dz;
            const double half = (face < 2) ? leg_w : leg_d;
            const double off  = ((face & 1) ? -1.0 : 1.0)
                              * ((face < 2) ? leg_d : leg_w);
            qx[0] = cxx + ax * half + ox * off; qz[0] = czz + az * half + oz * off;
            qx[1] = cxx - ax * half + ox * off; qz[1] = czz - az * half + oz * off;
            qx[2] = qx[1];                      qz[2] = qz[1];
            qx[3] = qx[0];                      qz[3] = qz[0];
            qy[0] = y0; qy[1] = y0; qy[2] = y1; qy[3] = y1;
            /* [R5 item 1] On the dedicated leg page the whole 64x64 is concrete,
             * so map the face across the page inset by half a texel (no border
             * fetch). Without the fix, the legacy 0..0.12 window of the BANNER
             * page -- the source of the reported edge bleed. */
            tg_gantry_quad(px, py, pz, uu, vv, &n, qx, qy, qz,
                           legfix ? lue : 0.0, legfix ? 1.0 - lue : 0.12,
                           legfix ? 1.0 - lue : 1.0, legfix ? lue : 0.0);
        }
    }
    cap_n = n;   /* verts so far are all legs; the cap follows on its own page */

    /* Underside cap of the panel, emitted here so the whole steel FRAME (legs +
     * cap) is one contiguous run of quads and therefore one command. */
    {
        const double y0 = base_y + TD5_TG_GANTRY_CLEAR;
        const double ox = lx + dx * out;
        const double oz = lz + dz * out;
        const double kx = rx - dx * out;
        const double kz = rz - dz * out;
        qx[0] = ox + tx * TD5_TG_GANTRY_THICK; qz[0] = oz + tz * TD5_TG_GANTRY_THICK;
        qx[1] = kx + tx * TD5_TG_GANTRY_THICK; qz[1] = kz + tz * TD5_TG_GANTRY_THICK;
        qx[2] = kx - tx * TD5_TG_GANTRY_THICK; qz[2] = kz - tz * TD5_TG_GANTRY_THICK;
        qx[3] = ox - tx * TD5_TG_GANTRY_THICK; qz[3] = oz - tz * TD5_TG_GANTRY_THICK;
        qy[0] = y0; qy[1] = y0; qy[2] = y0; qy[3] = y0;
        tg_gantry_quad(px, py, pz, uu, vv, &n, qx, qy, qz, 0.0, 1.0, 0.9, 1.0);
    }
    /* [R5 item 1] With the leg fix the legs are one command on the LEG page and
     * the underside cap stays on the BANNER page (its chequer end block); the
     * two word halves follow. Without it, legs+cap are one BANNER command as
     * before. */
    if (legfix) {
        seg_page[0] = TD5_TG_PAGE_R5_LEG;  seg_nq[0] = cap_n / 4;       /* legs */
        seg_page[1] = TD5_TG_PAGE_BANNER;  seg_nq[1] = (n - cap_n) / 4; /* cap  */
        nseg = 4;
    } else {
        seg_page[0] = TD5_TG_PAGE_BANNER;
        seg_nq[0]   = n / 4;
        nseg = 3;
    }

    /* --- panel: a slab bridging the two legs, carrying the SHIPPED START or
     * FINISH artwork. Front and back faces are separated by
     * TD5_TG_GANTRY_THICK so they cannot z-fight.
     *
     * [FB r2 item 12] The panel used to be a procedural chequer. Shipped TD5
     * banners are photographic pages that spell the word out, and the word does
     * not fit one 64x64 page: Keswick splits START over pages 337+338 and
     * FINISH over 369+370 (each with its own chequer end block), laid side by
     * side. So the panel is TWO half quads per face, each mapping one whole
     * page -- never one page stretched across the road, which is the mistake
     * the facade survey warned about.
     *
     * [FIX 2026-08-27 -- BANNER READ "TRATS" IN FRAME] There used to be a BACK
     * face too, carrying the same word readable from behind (halves swapped
     * pages and u reversed, because from behind the world-right half is what a
     * viewer sees on their left). It was separated from the front by
     * TD5_TG_GANTRY_THICK and the comment here claimed that meant they "cannot
     * z-fight". They can, and they did: THICK is 160 raw = 0.625 world units,
     * and the banner is first seen from tens of thousands of units away against
     * a 195000 far plane, so that separation is well inside depth-buffer noise.
     * The back face won, and since its halves are page-swapped AND u-reversed,
     * what you read from the car was the whole word mirrored: "TRATS".
     * Framedump: log/R_post1.png.
     *
     * This is the same failure the shipped-track banners hit (Keswick page 338,
     * Blue Ridge page 156, both z-fighting under the wrapper's global
     * CULL_MODE_NONE); there the cure was one-sided culling, NOT more
     * separation -- which is the evidence that separation does not hold up here.
     *
     * So the back face is gone. A start/finish gantry is read by a driver
     * APPROACHING the line, the back face served only legibility from behind
     * (which nobody needs while racing the right way round), and dropping it
     * makes the z-fight impossible by construction instead of merely unlikely.
     * Scenery is submitted CULL_NONE, so the single remaining face is still
     * drawn from both sides -- it just reads mirrored from behind, exactly like
     * the shipped one-sided banners do. Two half quads, one per page, emitted
     * in page order so each page stays one contiguous command run. --- */
    {
        const double y0 = base_y + TD5_TG_GANTRY_CLEAR;
        const double y1 = y0 + TD5_TG_GANTRY_PANEL_H;
        const double ox = lx + dx * out;   /* left  end */
        const double oz = lz + dz * out;
        const double kx = rx - dx * out;   /* right end */
        const double kz = rz - dz * out;
        const double mx = 0.5 * (ox + kx), mz = 0.5 * (oz + kz);
        /* (a-end, b-end, face sign, u at a, u at b) for the two half quads.
         * Row order IS the page order: row 0 is the L page, row 1 the R page,
         * which is what makes each page one command. */
        const double ends[2][5] = {
            { 0.0, 1.0,  1.0, 0.0, 1.0 },   /* front, left  half */
            { 1.0, 2.0,  1.0, 0.0, 1.0 }    /* front, right half */
        };
        const double ptx[3] = { ox, mx, kx };
        const double ptz[3] = { oz, mz, kz };
        int q;
        for (q = 0; q < 2; q++) {
            const int    ia = (int)ends[q][0], ib = (int)ends[q][1];
            const double sf = ends[q][2];
            qx[0] = ptx[ia] + tx * sf * TD5_TG_GANTRY_THICK;
            qz[0] = ptz[ia] + tz * sf * TD5_TG_GANTRY_THICK;
            qx[1] = ptx[ib] + tx * sf * TD5_TG_GANTRY_THICK;
            qz[1] = ptz[ib] + tz * sf * TD5_TG_GANTRY_THICK;
            qx[2] = qx[1]; qz[2] = qz[1];
            qx[3] = qx[0]; qz[3] = qz[0];
            qy[0] = y0; qy[1] = y0; qy[2] = y1; qy[3] = y1;
            /* [R4 item 1] Inset the panel UVs by half a texel. Each half maps
             * one whole 64x64 page (u 0->1); at the panel centre the L page's
             * right edge (u=1) abuts the R page's left edge (u=0), and sampling
             * those extreme columns -- wrapped, they fetch the OPPOSITE edge --
             * draws the reported vertical line. e0..e1 keeps every fetch off the
             * page border. ins=0 restores the raw 0/1 for the A/B. */
            tg_gantry_quad(px, py, pz, uu, vv, &n, qx, qy, qz,
                           ends[q][3] + (ends[q][4] - ends[q][3]) * ins,
                           ends[q][4] - (ends[q][4] - ends[q][3]) * ins,
                           1.0 - ins, ins);
        }
        /* Panel halves are the last two commands; their index shifts by one when
         * the legs took their own page ahead of the cap ([R5 item 1]). */
        {
            const int ps = legfix ? 2 : 1;
            seg_page[ps]     = finish ? TD5_TG_PAGE_FINISH_L : TD5_TG_PAGE_START_L;
            seg_page[ps + 1] = finish ? TD5_TG_PAGE_FINISH_R : TD5_TG_PAGE_START_R;
            seg_nq[ps]       = 1;  /* one front half per page (back face dropped) */
            seg_nq[ps + 1]   = 1;
        }
    }

    tg_acct(TG_ACCT_BANNER, si);
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, seg_page, seg_nq, nseg);
}

/* Group E -- track furniture: start/finish banners, branch mouths, run-off.
 * Exactly two gantries per track: one on the grid span and one on the finish
 * span (tg_finish_span, the same span the last LEVELINF checkpoint sits on --
 * so the banner is over the line that actually ends the race, not near it).
 * Default ON; TD5RE_AUTOTRACK_BANNERS=0 removes both. */
static int tg_emit_fb_track(const TG_FBHook *h)
{
    const int ring = (s_ring_len > 0) ? s_ring_len : h->nspans;
    const int finish = tg_finish_span(ring);

    if (h->si != TD5_TG_GRID_SPAN && h->si != finish) return 1;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_BANNERS")) return 1;
    if (*h->nmesh + 1 >= h->maxmesh) return 1;
    h->moff[(*h->nmesh)++] = h->blk->len;
    /* The finish span wins a tie: on a circuit the grid span and the finish can
     * be the same span, and that gantry marks the end of the race. */
    return tg_emit_gantry(h->nl, h->si, h->blk, h->si == finish);
}

/* [R3 item 19] Visible backstop wall across the full road at each OPEN end of
 * the strip, so the invisible boundary sentinel (td5_track_bind_boundary_
 * sentinels sets fwd=2 / rev=ring-3 for a custom P2P track) has something to
 * look like. The autotrack centreline is an open path, so those two spans are
 * the real map edges. `at_far` picks which edge of the span to draw on: 0 = the
 * near edge (start end, behind the grid), 1 = the far edge (finish end). One
 * opaque quad on the solid facade page (TD5_TG_PAGE_WALL) -- no new art. */
#define TD5_TG_ENDWALL_H 3200.0   /* raw; ~12.5 wu, over any jump the car makes */

static int tg_emit_end_wall(const TG_NodeList *nl, int si, int at_far,
                            size_t *moff, int *nmesh, int maxmesh, TG_Buf *blk)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    double lx, ly, lz, rx, ry, rz;
    int seg_page = TD5_TG_PAGE_WALL, seg_nq = 1;
    const double f = at_far ? 1.0 : 0.0;

    if (si < 0 || si + 1 >= nl->count) return 1;
    if (*nmesh >= maxmesh) return 1;
    tg_road_edge(nl, si, f, 0.0, 1.0, &lx, &ly, &lz, &rx, &ry, &rz);

    /* Quad loop: left-bottom, right-bottom, right-top, left-top. u spans the
     * road width twice so the brick reads at a believable scale; v 0..1 up. */
    px[0] = lx; py[0] = ly;                    pz[0] = lz; uu[0] = 0.0; vv[0] = 1.0;
    px[1] = rx; py[1] = ry;                    pz[1] = rz; uu[1] = 2.0; vv[1] = 1.0;
    px[2] = rx; py[2] = ry + TD5_TG_ENDWALL_H; pz[2] = rz; uu[2] = 2.0; vv[2] = 0.0;
    px[3] = lx; py[3] = ly + TD5_TG_ENDWALL_H; pz[3] = lz; uu[3] = 0.0; vv[3] = 0.0;

    moff[(*nmesh)++] = blk->len;
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1);
}

/* Fork whose MAIN half-carriageway covers main-ring span si, or -1. */
static int tg_fork_of_main(int si)
{
    int i;
    for (i = 0; i < s_fork_count; i++)
        if (si > s_forks[i].F && si <= s_forks[i].F + s_forks[i].len) return i;
    return -1;
}

/* Fork whose appended CORRIDOR covers span si (sets *k = corridor step), or -1. */
static int tg_fork_of_corridor(int si, int *k)
{
    int i;
    for (i = 0; i < s_fork_count; i++) {
        int lo = s_forks[i].cbase, hi = lo + s_forks[i].len - 1;
        if (si >= lo && si <= hi) { if (k) *k = si - lo; return i; }
    }
    return -1;
}

static int tg_emit_models(const TG_NodeList *nl, int nspans, int lanes,
                          TG_Buf *out)
{
    /* `nspans` is the FULL strip span count. With branches it INCLUDES each
     * fork's pad + corridor tail; the main ring is s_ring_len and the corridors
     * are appended after it (fork descriptors in s_forks). The centerline `nl`
     * only has the main-ring nodes, so a corridor span takes its geometry from
     * the base main node nl->v[F+1+k] plus the branch shift. */
    const int branch_active = tg_branches_enabled() && s_fork_count > 0;
    const int ring = branch_active ? s_ring_len : nspans;
    /* Native-faithful fork: the road SPLITS into two half-width carriageways --
     * MAIN (left, main_half lanes, +width/4) and BRANCH (right, br_lanes, bowed)
     * over the appended corridor. Fork/rejoin spans stay full width. */
    const int main_half = lanes / 2;
    const int br_lanes  = lanes - main_half;
    const int nentries = (nspans + TD5_TG_SPANS_PER_ENTRY - 1)
                       / TD5_TG_SPANS_PER_ENTRY;
    /* Per span: ground skirt + road + guardrail + building + up to 3 tunnel
     * pieces + up to 2 bridge pieces + several prop billboards. */
    /* 96 per span, raised from 48 once all five scenery areas of the 2026-08-26
     * batch were emitting at once: a built city span can carry ground + road +
     * gore + rail + facade + storefront + sidewalk + kerb + railing + crossing
     * + 6 lamp pieces + 4 back-row rows + treeline + far band + props, and the
     * budget is per ENTRY of TD5_TG_SPANS_PER_ENTRY spans, so the worst case is
     * four such spans in a row. Overflow is SILENT (the loop just stops adding
     * scenery), which is why it is counted and logged below rather than trusted.
     * Cost is stack only: moff is 96*4*8 = 3 KB. */
    enum { TG_MAX_MESHES_PER_ENTRY = TD5_TG_SPANS_PER_ENTRY * 96 };
    const int rails = tg_guardrails_enabled();
    int nrails = 0;
    int nbudget = 0;                /* entries that ran out of mesh slots */
    TG_Buf *blocks;
    unsigned int cursor;
    int e, ok = 1;

    blocks = (TG_Buf *)calloc((size_t)nentries, sizeof(TG_Buf));
    if (!blocks) return 0;

    s_guard_rejects = 0;   /* [R7 GUARD] per-build tally, reported below */
    s_guard_residual = 0;
    s_guard_ex_scope_hits = 0;
    memset(s_guard_rej_kind, 0, sizeof s_guard_rej_kind);

    for (e = 0; e < nentries && ok; e++) {
        const int s0 = e * TD5_TG_SPANS_PER_ENTRY;
        int ns = nspans - s0;
        size_t moff[TG_MAX_MESHES_PER_ENTRY];
        TG_Buf meshes;
        int nmesh = 0, i;

        if (ns > TD5_TG_SPANS_PER_ENTRY) ns = TD5_TG_SPANS_PER_ENTRY;
        memset(&meshes, 0, sizeof(meshes));
        tg_guard_ex_reset();   /* [R7 GUARD] exempt ranges are per-entry */

        /* Ground skirt then road, per span. Offsets are RECORDED as meshes are
         * appended -- sizes differ once ground, buildings and road quads are
         * mixed, so they cannot come from a uniform stride. */
        for (i = 0; i < ns && ok; i++) {
            const int si = s0 + i;

            /* Appended corridor span: the BRANCH (right) half carriageway only,
             * at the bowed geometry of whichever fork owns it. The pad span
             * (si == cbase-1) carries nothing. */
            if (si >= ring) {
                int ck = 0, fi = branch_active ? tg_fork_of_corridor(si, &ck) : -1;
                if (fi >= 0) {
                    const int mb = s_forks[fi].F + 1 + ck;  /* base main node */
                    const int L  = s_forks[fi].len;
                    /* Same lane/width helpers the STRIP rows used, so the
                     * surface you see is the surface you collide with even where
                     * the corridor gains a lane. */
                    const double sep = s_forks[fi].sep;
                    const double wn = tg_branch_wscale_s(ck, L, br_lanes, sep);
                    const double wf = tg_branch_wscale_s(ck + 1, L, br_lanes, sep);
                    /* Items 7 & 11: u_scale is CONSTANT along the corridor -- the
                     * base half carriageway (wscale 0.5) carries br_lanes lanes,
                     * so u_scale = br_lanes/0.5 = 2*br_lanes gives U = br_lanes at
                     * the base and one fixed-pitch lane stripe per real lane as it
                     * widens. It does NOT depend on this span's lane count, so the
                     * paint neither stretches with the width taper nor jumps when
                     * a lane is gained (both were the old flat max(ln,lnf) U). */
                    const double u_scale = 2.0 * (double)br_lanes;
                    moff[nmesh++] = meshes.len;
                    /* Widths near/far, NOT the wider of the two: the strip rows
                     * taper across the span (see the corridor loop in
                     * tg_emit_strip) and the mesh has to taper with them or the
                     * surface you see stops being the surface you collide with. */
                    if (!tg_emit_road_quad_taper(nl, mb, u_scale,
                                           tg_branch_shift_s(ck, L, nl->v[mb].width, sep),
                                           tg_branch_shift_s(ck + 1, L, nl->v[mb + 1].width, sep),
                                           wn, wf, tg_road_page(mb), &meshes))
                        ok = 0;
                    /* [R7 GUARD] the branch carriageway is drivable; it sits deep
                     * inside the -side reach envelope, so mark it exempt. */
                    tg_guard_mark(moff[nmesh - 1], meshes.len, TG_GK_BRANCHROAD, si);
                    /* Item 9a: the branch carriageway had NO kerb of its own, so
                     * driving a corridor there was road meeting bare ground with
                     * no pavement -- the "missing gaps between road and sidewalk
                     * on branches". Lay a pavement along the branch's OUTER edge,
                     * derived from the SAME shift/width the road just used so it
                     * follows the bow and the widening. Paved biomes only, same
                     * rule the main-ring sidewalk uses. */
                    if (ok) {
                        const TG_Biome *cb = &k_biomes[tg_biome_for_span(mb)];
                        if (tg_city_sidewalk_w(cb) > 0.0 &&
                            td5_env_flag_on("TD5RE_AUTOTRACK_SIDEWALKS")) {
                            if (!tg_emit_branch_sidewalk(nl, mb, ck, L, sep,
                                                         br_lanes, cb, &meshes,
                                                         moff, &nmesh, si)) ok = 0;
                        /* [R5 item 10] Out-of-town corridor: no city pavement, so
                         * give the outer edge the SAME flat verge band the main
                         * road gets in that biome. Without this a fork through a
                         * tree biome (seed 99991 fork 2 through ORIENTAL) had a
                         * bare right edge -- "no sidewalk on the right of the
                         * branch". */
                        } else if (tg_verge_band_w(cb) > 0.0 &&
                                   td5_env_flag_on("TD5RE_R5_BRANCH_VERGE")) {
                            if (!tg_emit_branch_verge(nl, mb, ck, L, sep,
                                                      br_lanes, tg_verge_band_w(cb),
                                                      &meshes, moff, &nmesh, si)) ok = 0;
                        }
                        /* [R7 item 10] Dress the grass verge with the biome's own
                         * roadside trees. No-op on paved biomes (no billboard set)
                         * and inherently clear of the branch (clear_gap). */
                        if (ok && !tg_emit_branch_flora(nl, mb, cb, &meshes,
                                                        moff, &nmesh, si)) ok = 0;
                    }
                }
                /* The other appended span is the PAD (si == cbase-1). It is
                 * DEGENERATE by construction -- both of its rows are node F --
                 * and the fork span's own full-width quad already covers that
                 * ground, so giving it a mesh would only z-fight (the Keswick
                 * start-banner lesson). It stays geometry-free on purpose. */
                continue;
            }

            moff[nmesh++] = meshes.len;
            {
                const TG_Biome *gb = &k_biomes[tg_biome_for_span(si)];
                double wsd = gb->water ? tg_water_side(si) : 0.0;
                if (!tg_emit_ground(nl, si, &meshes, wsd)) { ok = 0; break; }
            }
            /* [R7 GUARD] the ground skirt underlaps the road by design. */
            tg_guard_mark(moff[nmesh - 1], meshes.len, TG_GK_SKIRT, si);
            {
            const size_t road_ex0 = meshes.len;   /* [R7 GUARD] road+gore+divider */
            moff[nmesh++] = meshes.len;
            /* MAIN carriageway is narrowed to the LEFT half over each fork's
             * region [F+1 .. F+len]; elsewhere (incl. the full-width fork and
             * rejoin spans) it is the plain full road. */
            {
                int fi = branch_active ? tg_fork_of_main(si) : -1;
                if (fi >= 0) {
                    const int L = s_forks[fi].len;
                    const int j = si - s_forks[fi].F - 1;   /* corridor step */
                    if (!tg_emit_road_quad(nl, si, main_half,
                                           TD5_TG_MAIN_SHIFT(nl->v[si].width),
                                           TD5_TG_MAIN_SHIFT(nl->v[si + 1].width),
                                           0.5, tg_road_page(si), &meshes))
                        ok = 0;
                    if (ok) {
                        /* Branch half widths from the SAME helper the corridor
                         * strip rows and mesh use, so the gore always meets the
                         * branch's left edge however wide the taper has made it. */
                        const double sep = s_forks[fi].sep;
                        const double sh0 = tg_branch_shift_s(j, L, nl->v[si].width, sep);
                        const double sh1 = tg_branch_shift_s(j + 1, L, nl->v[si + 1].width, sep);
                        const double gw0 = nl->v[si].width
                            * tg_branch_wscale_s(j, L, br_lanes, sep) * 0.5;
                        const double gw1 = nl->v[si + 1].width
                            * tg_branch_wscale_s(j + 1, L, br_lanes, sep) * 0.5;
                        moff[nmesh++] = meshes.len;
                        /* [R7 item 12] Median surface stable per fork, not the
                         * blended per-span biome that dithered grass/tiles at a
                         * boundary crossing the fork. TD5RE_R7_MEDIAN_STABLE=0
                         * restores the per-span page for an A/B. */
                        {
                            const int gore_page =
                                td5_env_flag_on("TD5RE_R7_MEDIAN_STABLE")
                                    ? tg_fork_gore_page(fi)
                                    : k_biomes[tg_biome_for_span(si)].ground_page;
                            if (!tg_emit_gore(nl, si, sh0, sh1, gw0, gw1,
                                              gore_page, &meshes))
                                ok = 0;
                        }
                        /* Items 9c/10: where the fork is TIGHT (an avenue) the
                         * gore is a slim central median, not a wide split -- give
                         * it a raised divider so the two carriageways read as one
                         * divided avenue. Treatment varies per fork. Emitted on
                         * the MAIN fork span alongside the gore it sits on. */
                        if (ok && tg_fork_is_avenue(fi) &&
                            td5_env_flag_on("TD5RE_AUTOTRACK_AVENUE_DIVIDER"))
                            if (!tg_emit_avenue_divider(nl, si, fi, sh0, sh1,
                                                        gw0, gw1, &meshes,
                                                        moff, &nmesh)) ok = 0;
                    }
                } else if (!tg_emit_road_mesh(nl, si, lanes, &meshes)) {
                    ok = 0;
                }
            }
            /* [R7 GUARD] road quads, the fork gore and the avenue divider all
             * occupy the drivable envelope on purpose. */
            tg_guard_mark(road_ex0, meshes.len, TG_GK_ROAD, si);
            }
            /* Guardrails belong in THIS loop, not the box pass below: that pass
             * recovers each piece's offset by dividing the appended bytes by
             * n_added, which only holds while every piece is a same-sized box.
             * A rail prism has a different vertex count and would silently
             * corrupt those offsets. Here each offset is recorded explicitly. */
            if (ok && rails &&
                tg_span_needs_guardrail(nl, si, nspans)) {
                const size_t r0 = meshes.len;
                moff[nmesh++] = meshes.len;
                if (!tg_emit_guardrail(nl, si, &meshes)) ok = 0;
                else nrails++;
                tg_guard_mark(r0, meshes.len, TG_GK_RAIL, si);
            }
        }
        for (i = 0; i < ns && ok; i++) {
            const int si = s0 + i;
            int k;

            if (si >= ring) continue;   /* pad + corridor carry road only */
            /* Headroom for the widest single span (see the budget note above).
             * Hitting this is not an error, but it drops the rest of the entry's
             * scenery with no other symptom, so COUNT it -- a silent hole in the
             * world is the hardest kind of bug to chase from a screenshot. */
            if (nmesh + 96 > TG_MAX_MESHES_PER_ENTRY) { nbudget++; break; }

            {   /* [FB] scenery hooks: one call per work area. `hook` is rebuilt
                 * per span so *nmesh always tracks the live counter. */
                TG_FBHook hook;
                hook.nl = nl; hook.si = si; hook.nspans = nspans;
                hook.lanes = lanes;
                hook.b = &k_biomes[tg_biome_for_span(si)];
                hook.blk = &meshes; hook.moff = moff; hook.nmesh = &nmesh;
                hook.maxmesh = TG_MAX_MESHES_PER_ENTRY;
                if (tg_span_in_tunnel(si)) {
                    /* [R7 GUARD] the tunnel-portal mountain massing sits above and
                     * beside the bore on purpose. */
                    size_t t0 = meshes.len;
                    if (!tg_emit_fb_tunnel(&hook)) { ok = 0; break; }
                    tg_guard_mark(t0, meshes.len, TG_GK_TUNNEL, si);
                } else {
                    /* [R8 GUARD] Kind marks on the NON-exempt hooks too. These
                     * license nothing (every kind below is SCENERY class); they
                     * exist so a rejection can name the emitter that produced
                     * the mesh, which is what makes "rejections broken down by
                     * kind, and zero of them road/deck/gantry/tunnel" a number
                     * in the log rather than a claim. */
                    size_t g0 = meshes.len;
                    if (!tg_emit_fb_city(&hook))    { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_CITY, si);
                    g0 = meshes.len;
                    if (!tg_emit_fb_block(&hook))   { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_BLOCK, si);
                    g0 = meshes.len;
                    if (!tg_emit_fb_cross(&hook))   { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_CROSS, si);
                    g0 = meshes.len;
                    if (!tg_emit_fb_flora(&hook))   { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_FLORA, si);
                    g0 = meshes.len;
                    if (!tg_emit_fb_park_trees(&hook)) { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_PARKTREE, si);
                    g0 = meshes.len;
                    if (!tg_emit_fb_terrain(&hook)) { ok = 0; break; }
                    tg_guard_mark(g0, meshes.len, TG_GK_TERRAIN, si);
                }
                /* [R7 GUARD] the start/finish gantry legs stand at the road edge
                 * and its beam spans overhead -- authored across the road. */
                {
                    size_t k0 = meshes.len;
                    if (!tg_emit_fb_track(&hook)) { ok = 0; break; }
                    tg_guard_mark(k0, meshes.len, TG_GK_GANTRY, si);
                }
            }

            /* [R3 item 19] Backstop walls at the two OPEN ends of the strip,
             * aligned with the collision boundary sentinels (fwd=2, rev=ring-3).
             * Emitted here so a tunnel span at either end still gets its cap. */
            if (si == 2 || si == ring - 3) {
                size_t ew0 = meshes.len;
                if (!tg_emit_end_wall(nl, si, si != 2, moff, &nmesh,
                                      TG_MAX_MESHES_PER_ENTRY, &meshes)) {
                    ok = 0; break;
                }
                /* [R7 GUARD] the strip's two end backstop walls span the road on
                 * purpose (the collision sentinels behind the start/finish). */
                tg_guard_mark(ew0, meshes.len, TG_GK_ENDWALL, si);
            }

            if (tg_span_in_tunnel(si)) {
                /* Enclosed: no buildings, they would stand inside the walls.
                 * Tunnel pieces are equal-sized boxes, so recover each from the
                 * appended span. */
                size_t before = meshes.len;
                int n_added = 0;
                if (!tg_emit_tunnel(nl, si, &meshes, &n_added)) { ok = 0; break; }
                for (k = 0; k < n_added; k++)
                    moff[nmesh++] = before + (size_t)k *
                                    ((meshes.len - before) / (size_t)n_added);
                /* [R7 GUARD] the tunnel bore encloses the road. */
                tg_guard_mark(before, meshes.len, TG_GK_TUNNEL, si);
            } else {
                const TG_Biome *b = &k_biomes[tg_biome_for_span(si)];
                size_t b0 = meshes.len, b1;
                int nb = 0;
                /* Building: 0 or 1 mesh -- record its offset explicitly. */
                if (!tg_building_for_span(nl, si, &meshes)) { ok = 0; break; }
                if (meshes.len > b0) moff[nmesh++] = b0;
                tg_guard_mark(b0, meshes.len, TG_GK_BUILDING, si);
                /* Bridge: 0..N equal-sized boxes among themselves.
                 * [R7 GUARD] the deck IS the road and the piers descend from it;
                 * the parapets/ribs sit on the deck edge or overhead. */
                b1 = meshes.len;
                if (!tg_emit_bridge(nl, si, &meshes, &nb)) { ok = 0; break; }
                for (k = 0; k < nb; k++)
                    moff[nmesh++] = b1 + (size_t)k *
                                    ((meshes.len - b1) / (size_t)nb);
                tg_guard_mark(b1, meshes.len, TG_GK_DECK, si);
                /* Sloped parapets + optional overhead ribs (items 12, 13):
                 * separate quad meshes, each records its own offset. */
                {
                    size_t br0 = meshes.len;
                    if (!tg_emit_bridge_rails(nl, si, &meshes, moff, &nmesh)) {
                        ok = 0; break;
                    }
                    tg_guard_mark(br0, meshes.len, TG_GK_DECK, si);
                }
                /* River under a bridge run. [R7 GUARD] the water is the surface a
                 * bridge deck flies over -- never scenery on the road. */
                if (tg_span_in_bridge_run(si)) {
                    size_t bw0 = meshes.len;
                    if (!tg_emit_bridge_water(nl, si, &meshes, moff, &nmesh)) {
                        ok = 0; break;
                    }
                    tg_guard_mark(bw0, meshes.len, TG_GK_WATER, si);
                }
                /* [R4 item 20] Coastline at the river's longitudinal ends. */
                if (tg_span_in_bridge_run(si)) {
                    size_t bc0 = meshes.len;
                    if (!tg_emit_bridge_coast(nl, si, &meshes, moff, &nmesh)) {
                        ok = 0; break;
                    }
                    tg_guard_mark(bc0, meshes.len, TG_GK_COAST, si);
                }
                /* Prop billboards: variable count/size, each records its own. */
                {
                    size_t p0 = meshes.len;
                    if (!tg_emit_props(nl, si, b, &meshes, moff, &nmesh,
                                       TG_MAX_MESHES_PER_ENTRY)) { ok = 0; break; }
                    tg_guard_mark(p0, meshes.len, TG_GK_PROP, si);
                }
                /* Sea plane on coastal runs. [R6 item 14] NOT on a bridge run:
                 * the full-width bridge water already covers the surface there
                 * (now at sea level), and laying the one-sided sea plane over it
                 * too gave two overlapping surfaces -- the z-fighting seam and
                 * the level step of "the water is not continuous". */
                if (b->water && !tg_span_in_bridge_run(si)) {
                    size_t sw0 = meshes.len;
                    if (!tg_emit_water(nl, si, tg_water_side(si), &meshes,
                                       moff, &nmesh)) { ok = 0; break; }
                    /* [R7 GUARD] the sea plane sits beside/below the road. */
                    tg_guard_mark(sw0, meshes.len, TG_GK_WATER, si);
                }
            }
        }

        /* [R7 GUARD] Post-emit backstop: reject any scenery mesh standing in the
         * carriageway, over ALL of this entry's assembled geometry, whatever
         * emitter produced it. Runs before the block header so the rejected
         * meshes never reach the offset table. */
        if (ok)
            tg_guard_validate_entry(nl, ring, s0, ns, &meshes, moff, &nmesh);

        if (ok) {
            const unsigned int hdr = (unsigned)(4 + nmesh * 4);
            tg_put_u32(&blocks[e], (unsigned)nmesh);
            for (i = 0; i < nmesh; i++)
                tg_put_u32(&blocks[e], hdr + (unsigned)moff[i]);
            if (!tg_buf_need(&blocks[e], meshes.len)) ok = 0;
            else {
                memcpy(blocks[e].b + blocks[e].len, meshes.b, meshes.len);
                blocks[e].len += meshes.len;
            }
        }
        tg_buf_free(&meshes);
    }

    if (ok) {
        /* Header: count, then (offset,size) pairs. Block 0 must begin exactly
         * at 4 + count*8 -- the strict-format-A autodetect requires it. */
        cursor = (unsigned)(4 + nentries * 8);
        tg_put_u32(out, (unsigned)nentries);
        for (e = 0; e < nentries; e++) {
            tg_put_u32(out, cursor);
            tg_put_u32(out, (unsigned)blocks[e].len);
            cursor += (unsigned)blocks[e].len;
        }
        for (e = 0; e < nentries; e++) {
            if (!tg_buf_need(out, blocks[e].len)) { ok = 0; break; }
            memcpy(out->b + out->len, blocks[e].b, blocks[e].len);
            out->len += blocks[e].len;
        }
        if (ok) {
            TD5_LOG_I(LOG_TAG, "trackgen: models = %d entries, %zu bytes "
                      "(%d road meshes)", nentries, out->len, nspans);
            /* Report coverage rather than assuming the gate is sane: a rail
             * count of 0 or of nspans both mean the curvature threshold is
             * wrong, and that is invisible without a number. */
            if (rails)
                TD5_LOG_I(LOG_TAG, "trackgen: guardrails on %d/%d spans (%d%%)",
                          nrails, nspans,
                          nspans ? (nrails * 100 / nspans) : 0);
            /* WARN, not INFO: any non-zero count means some spans are missing
             * scenery they were meant to have, and nothing else would say so. */
            if (nbudget)
                TD5_LOG_W(LOG_TAG, "trackgen: mesh budget exhausted in %d/%d "
                          "entries -- those spans lost scenery (raise "
                          "TG_MAX_MESHES_PER_ENTRY)", nbudget, nentries);
            /* [R7 GUARD] class-level evidence: a non-zero count is a real number
             * of scenery meshes that WOULD have stood in the road, dropped (or,
             * in report-only mode, logged) by the post-emit backstop. "clean"
             * means the whole strip validated against the carriageway. */
            if (!tg_guard_enabled())
                TD5_LOG_I(LOG_TAG, "on-road guard: DISABLED (TD5RE_R7_GUARD=0)");
            else if (s_guard_rejects)
                TD5_LOG_W(LOG_TAG, "on-road guard: %ld scenery mesh(es) %s the "
                          "carriageway across %d spans%s; %ld remaining after "
                          "the pass", s_guard_rejects,
                          tg_guard_report_only() ? "would overlap"
                                                 : "rejected for overlapping",
                          nspans, tg_guard_report_only() ? " (REPORT-ONLY)" : "",
                          s_guard_residual);
            else
                TD5_LOG_I(LOG_TAG, "on-road guard: clean -- 0 scenery meshes in "
                          "the road over %d spans (%ld remaining)", nspans,
                          s_guard_residual);
            if (s_guard_residual && !tg_guard_report_only())
                TD5_LOG_W(LOG_TAG, "on-road guard: RESIDUAL %ld non-exempt "
                          "on-road mesh(es) survived -- parse/compaction bug",
                          s_guard_residual);
            /* [R8] The acceptance evidence for the precision pass, as numbers.
             * ONE line per kind that was rejected, and an explicit line for the
             * kinds that must NEVER appear: if road / deck / gantry / tunnel /
             * skirt / water / branch-road ever show a non-zero count, the guard
             * has started eating the track and that is a stop-the-line failure,
             * not a tuning question. */
            if (tg_guard_enabled()) {
                /* The kinds that must NEVER be rejected: the drivable surface
                 * and the structures that enclose it. NOT skirt/water/coast --
                 * those are UNDER class, and rejecting one that has climbed ON
                 * TOP of the carriageway is the R8 item-7 fix, not a failure. */
                static const int k_never[] = {
                    TG_GK_ROAD, TG_GK_BRANCHROAD, TG_GK_DECK, TG_GK_GANTRY,
                    TG_GK_TUNNEL, TG_GK_ENDWALL
                };
                long never = 0;
                int ki;
                for (ki = 0; ki < TG_GK_COUNT; ki++)
                    if (s_guard_rej_kind[ki])
                        TD5_LOG_I(LOG_TAG, "on-road guard: rejects by kind: "
                                  "%-12s %ld", k_guard_kind_name[ki],
                                  s_guard_rej_kind[ki]);
                for (ki = 0; ki < (int)(sizeof k_never / sizeof k_never[0]); ki++)
                    never += s_guard_rej_kind[k_never[ki]];
                if (never)
                    TD5_LOG_W(LOG_TAG, "on-road guard: %ld TRACK-SURFACE mesh(es) "
                              "rejected (road/branch-road/deck/gantry/tunnel/"
                              "end-wall) -- the guard is eating the track", never);
                else
                    TD5_LOG_I(LOG_TAG, "on-road guard: 0 road/branch-road/deck/"
                              "gantry/tunnel/end-wall meshes rejected");
                if (s_guard_ex_scope_hits)
                    TD5_LOG_I(LOG_TAG, "on-road guard: %ld exempt mesh(es) fell "
                              "outside their own span run and were validated as "
                              "scenery", s_guard_ex_scope_hits);
            }
        }
    }

    for (e = 0; e < nentries; e++) tg_buf_free(&blocks[e]);
    free(blocks);
    return ok && !out->oom;
}

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

/* Page 0: asphalt with a lane line down one edge of the tile. The road mesh's
 * UVs run u = 0..lanes and tile, so a tile edge lands exactly on every lane
 * boundary -- a stripe at u=0 therefore draws lane dividers AND both road
 * edges for free, with no extra geometry. */
static void tg_emit_texture_page_asphalt(TG_Buf *out)
{
    unsigned int rng = 0x1234567u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);  /* pad[3] */
    tg_put_u8(out, 0);                                        /* type: opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* Palette is BGR. 0..11 asphalt greys, 12..15 near-white for the marking. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (44 + i * 3) : (196 + (i - 12) * 12);
        tg_put_u8(out, (unsigned)v);   /* B */
        tg_put_u8(out, (unsigned)v);   /* G */
        tg_put_u8(out, (unsigned)v);   /* R */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (x <= 1) {
            /* Lane marking, dashed down-track so it reads as road paint. */
            idx = ((y % 24) < 16) ? (12 + (int)((rng >> 16) % 4)) : 6;
        } else {
            idx = (int)((rng >> 16) % 12);   /* asphalt grain */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Page 1 (+ variants): building wall -- banded masonry with lit windows. The
 * `variant` seeds the concrete tone, window tint and window rhythm so the
 * procedural streetscape mixes several facades the way the real one does. */
static void tg_emit_texture_page_wall(TG_Buf *out, int variant)
{
    unsigned int rng = 0x9E3779B9u + (unsigned)variant * 0x2545F491u;
    int wcols  = 3 + (variant % 3);      /* window bays per cell: 3..5 */
    int warm   = (variant & 1) ? 24 : 0; /* some blocks read brick-warm */
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..9 concrete greys, 10..12 mortar shadow, 13..15 lit windows. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 10)      { b = 96 + i * 7;         g = 92 + i * 7;
                           r = 88 + i * 7 + warm;  }
        else if (i < 13) { b = 54;                 g = 52;
                           r = 50 + warm;          }
        else             { b = 150 + (i-13) * 30 - warm; g = 170 + (i-13) * 28;
                           r = 190 + (i-13) * 20 + warm;   }
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int cellpx = TD5_TG_TEX_DIM / (wcols + 1);   /* window cell pitch */
        int wx = (cellpx > 0) ? (x % cellpx) : x;
        int wy = y % 16;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (wy < 2 || wx < 2) {
            idx = 10 + (int)((rng >> 16) % 3);          /* storey / pier lines */
        } else if (wx >= 3 && wx <= cellpx - 3 && wy >= 5 && wy <= 12) {
            /* Window, lit or dark per cell so the facade is not uniform. */
            unsigned int cell = (unsigned)((y / 16) * 8 + (x / cellpx)) * 2654435761u;
            idx = ((cell >> 28) & 1) ? (13 + (int)((rng >> 18) % 3)) : 11;
        } else {
            idx = (int)((rng >> 16) % 10);               /* concrete */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Storefront pages: a glazed ground floor -- a coloured awning/sign band across
 * the top, big mullioned shop windows below with the odd lit pane, and a dark
 * doorway. `variant` recolours the awning (red/green/blue) so shops differ. */
static void tg_emit_texture_page_store(TG_Buf *out, int variant)
{
    static const int awn[3][3] = {           /* BGR awnings */
        { 40, 40, 170 }, { 60, 150, 60 }, { 160, 90, 40 } };
    unsigned int rng = 0x1234567u + (unsigned)variant * 0x9E3779B9u;
    int av = variant % 3, i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* 0..5 dark glass, 6..8 frame/mullion + doorway, 9..11 awning, 12..15
     * reflection / lit-sign highlights. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 6)       { b = 46 + i * 6;  g = 40 + i * 5;  r = 34 + i * 4; }
        else if (i < 9)  { b = 26;          g = 24;          r = 22;         }
        else if (i < 12) { b = awn[av][0];  g = awn[av][1];  r = awn[av][2]; }
        else             { b = 150 + (i-12) * 24; g = 160 + (i-12) * 22;
                           r = 170 + (i-12) * 18; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;              /* y=0 is the TOP of the page */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 12) {
            idx = 9 + (int)((rng >> 16) % 3);            /* awning / sign band */
        } else if (y > 46 && x > 26 && x < 38) {
            idx = 6 + (int)((rng >> 16) % 3);            /* dark doorway */
        } else if ((x % 16) < 2 || (y % 20) < 2) {
            idx = 6 + (int)((rng >> 16) % 3);            /* window frame/mullion */
        } else {
            unsigned int pane = (unsigned)((y / 20) * 4 + x / 16) * 2654435761u;
            idx = ((pane >> 29) == 0) ? (12 + (int)((rng >> 18) % 4))
                                      : (int)((rng >> 16) % 6);   /* lit / glass */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Page 2: foliage / vegetation -- mottled greens for hedgerows and treelines.
 * Deliberately noisy rather than structured: these boxes stand in for organic
 * mass, so any regular pattern reads as wrong. */
static void tg_emit_texture_page_green(TG_Buf *out)
{
    unsigned int rng = 0x51ED2701u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. Dark shadowed foliage up to sunlit leaf, with a little earth. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 12) { b = 28 + i * 3; g = 52 + i * 9; r = 24 + i * 4; }
        else        { b = 46;         g = 58;         r = 62 + (i-12) * 6; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int idx;
        rng = rng * 1103515245u + 12345u;
        /* Clumped rather than per-texel noise: bias by a coarse cell so the
         * canopy has light and dark masses instead of uniform static. */
        {
            int x = i % TD5_TG_TEX_DIM, y = i / TD5_TG_TEX_DIM;
            unsigned int cell = (unsigned)((y / 8) * 8 + (x / 8)) * 2654435761u;
            int bias = (int)((cell >> 29) % 5);
            idx = (int)((rng >> 16) % 8) + bias;
            if (idx > 15) idx = 15;
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Tree pages: an ALPHA-KEYED silhouette (type 1, index 0 = transparent key) so
 * the surround cuts out. `shape` selects the species outline so the procedural
 * fallback still mixes deciduous/conifer/palm/topiary/willow like the real set. */
static void tg_emit_texture_page_tree(TG_Buf *out, int shape)
{
    unsigned int rng = 0x2545F491u + (unsigned)shape * 0x9E3779B9u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 1);                                  /* 1 = alpha-keyed */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0 = key (never drawn), 1..3 trunk browns, 4..15 canopy greens
     * (olive-yellow for palm fronds). */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i == 0)                       { b = 255; g = 0;  r = 255; }
        else if (i < 4)                   { b = 30 + i * 6; g = 44 + i * 8; r = 62 + i * 10; }
        else if (shape == TG_TREE_PALM)   { b = 24 + i;     g = 70 + i * 8; r = 40 + i * 4;  }
        else                              { b = 26 + i * 2; g = 60 + i * 10; r = 22 + i * 3; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        /* y=0 is the TOP of the page; the billboard maps the base to v=1. */
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int dx = x - 32;
        int idx = 0;
        rng = rng * 1103515245u + 12345u;

        switch (shape) {
        case TG_TREE_CONIFER:      /* triangle widening to the base */
            if (y >= 52) { if (dx > -3 && dx < 3) idx = 1 + (int)((rng >> 17) % 3); }
            else { int hw = 2 + (y * 26) / 52 - (int)((rng >> 19) % 4);
                   if (dx > -hw && dx < hw) idx = 4 + (int)((rng >> 16) % 12); }
            break;
        case TG_TREE_PALM:         /* tall bare trunk, fronds fanning at the top */
            if (y >= 20) { if (dx > -2 && dx < 3) idx = 1 + (int)((rng >> 17) % 3); }
            else { int ry = y - 8, rad = 20 - (ry * ry) / 6 - (int)((rng >> 19) % 4);
                   if (dx * dx < rad * rad && ((x + y) & 3) != 0)
                       idx = 4 + (int)((rng >> 16) % 12); }
            break;
        case TG_TREE_TOPIARY:      /* tight round ball on a short stem */
            if (y >= 48) { if (dx > -2 && dx < 2) idx = 1 + (int)((rng >> 17) % 3); }
            else { int ry = y - 26, rad = 22 - (int)((rng >> 19) % 2);
                   if (dx * dx + ry * ry < rad * rad) idx = 4 + (int)((rng >> 16) % 8); }
            break;
        case TG_TREE_WILLOW:       /* wide drooping canopy with trailing strands */
            if (y >= 50) { if (dx > -3 && dx < 3) idx = 1 + (int)((rng >> 17) % 3); }
            else { int rad = 24 - (y / 4) - (int)((rng >> 19) % 3);
                   if (dx > -rad && dx < rad && (y < 30 || ((y + dx) & 1)))
                       idx = 4 + (int)((rng >> 16) % 12); }
            break;
        default:                   /* TG_TREE_DECID: rough blob */
            if (y >= 44) { if (dx > -4 && dx < 4) idx = 1 + (int)((rng >> 17) % 3); }
            else { int ry = y - 22, rad = 26 - (ry * ry) / 18 - (int)((rng >> 19) % 5);
                   if (dx * dx < rad * rad) idx = 4 + (int)((rng >> 16) % 12); }
            break;
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Procedural prop silhouettes (fallback when real textures are off): a person,
 * a stone statue, an animal, or a radial streetlamp glow. Alpha-keyed (index 0)
 * except the lamp, which is additive (type 3). Crude but recognisable. */
static void tg_emit_texture_page_prop(TG_Buf *out, int kind)
{
    unsigned int rng = 0x51ED2701u + (unsigned)kind * 0x9E3779B9u;
    int type = (kind == TG_PROP_LAMP) ? 3 : 1;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, (unsigned)type);
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i == 0)                     { b = 255; g = 0; r = 255; }   /* key */
        else if (kind == TG_PROP_LAMP)  { int v = 60 + i * 13; if (v > 255) v = 255;
                                          b = v; g = v; r = (v > 30 ? v - 30 : 0); }
        else if (kind == TG_PROP_STATUE){ int v = 70 + i * 11; if (v > 255) v = 255;
                                          b = v; g = v; r = v; }
        else if (kind == TG_PROP_ANIMAL){ b = 40 + i * 6; g = 44 + i * 7; r = 52 + i * 9; }
        else if (i < 8)                 { b = 40 + i * 4; g = 44 + i * 5; r = 60 + i * 8; }
        else                            { b = 90; g = 70; r = 60; }   /* person cloth */
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;              /* y=0 is the TOP */
        int dx = x - 32;
        int idx = 0;
        rng = rng * 1103515245u + 12345u;
        switch (kind) {
        case TG_PROP_LAMP: {
            int d2 = dx * dx + (y - 32) * (y - 32), rad = 28;
            if (d2 < rad * rad) { int t = 15 - (d2 * 15) / (rad * rad);
                                  idx = t < 1 ? 1 : t; }
            break; }
        case TG_PROP_ANIMAL:
            if (y >= 30 && y < 46 && dx > -16 && dx < 16) idx = 2 + (int)((rng >> 16) % 6);
            else if (y >= 46 && y < 58 && (x % 10) < 3)   idx = 2 + (int)((rng >> 16) % 4);
            break;
        case TG_PROP_STATUE:
            if (y >= 54) { if (dx > -14 && dx < 14) idx = 4 + (int)((rng >> 16) % 6); }
            else if (y >= 16) { if (dx > -8 && dx < 8) idx = 6 + (int)((rng >> 16) % 8); }
            else { if (dx > -5 && dx < 5) idx = 6 + (int)((rng >> 16) % 8); }
            break;
        default: /* TG_PROP_PERSON */
            if (y >= 8 && y < 18) { if (dx > -5 && dx < 5) idx = 9 + (int)((rng >> 16) % 3); }
            else if (y >= 18 && y < 40) { if (dx > -8 && dx < 8) idx = 1 + (int)((rng >> 16) % 7); }
            else if (y >= 40 && y < 60) { if ((dx > -8 && dx < -1) || (dx > 1 && dx < 8))
                                              idx = 1 + (int)((rng >> 16) % 5); }
            break;
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Water page: deep blue with lighter ripple crests and the odd foam fleck. */
static void tg_emit_texture_page_water(TG_Buf *out)
{
    unsigned int rng = 0x009E12D3u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR: 0..11 blue deepening, 12..15 foam highlight. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 12) { b = 150 + i * 8; g = 90 + i * 9;  r = 40 + i * 6; }
        else        { b = 235;         g = 210 + (i-12) * 10; r = 200 + (i-12) * 12; }
        if (b > 255) b = 255;
        if (g > 255) g = 255;
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int y = i / TD5_TG_TEX_DIM;
        int band, idx;
        rng = rng * 1103515245u + 12345u;
        band = (y + (int)((rng >> 20) % 3)) % 8;
        if (band < 2) idx = 8 + (int)((rng >> 16) % 4);      /* ripple crest */
        else          idx = (int)((rng >> 16) % 8);          /* blue */
        if (((rng >> 24) & 31) == 0) idx = 12 + (int)((rng >> 16) % 4); /* foam */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Road-surface pages for the themed biomes: gravel, dirt, ice, cobble. No lane
 * paint (only the base tarmac page carries markings). `kind` is an RS_* value. */
static void tg_emit_texture_page_roadsurf(TG_Buf *out, int kind)
{
    unsigned int rng = 0x00C0FFEEu + (unsigned)kind * 0x9E3779B9u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        switch (kind) {
        case RS_DIRT:   b = 40 + i * 5;  g = 60 + i * 6;  r = 80 + i * 8;  break;
        case RS_ICE:    b = 210 + i * 3; g = 205 + i * 3; r = 195 + i * 3; break;
        case RS_COBBLE: b = 78 + i * 7;  g = 76 + i * 7;  r = 74 + i * 7;  break;
        default:        b = 96 + i * 6;  g = 96 + i * 6;  r = 94 + i * 6;  break; /* gravel */
        }
        if (b > 255) b = 255;
        if (g > 255) g = 255;
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        switch (kind) {
        case RS_COBBLE: {                        /* stone blocks with mortar */
            int cx = x % 12, cy = y % 12;
            idx = (cx < 2 || cy < 2) ? 1 + (int)((rng >> 16) % 2)
                                     : 6 + (int)((rng >> 16) % 8);
            break; }
        case RS_ICE:                             /* pale, faint cracks */
            idx = 10 + (int)((rng >> 16) % 6);
            if (((rng >> 24) & 63) == 0) idx = 2 + (int)((rng >> 16) % 3);
            break;
        case RS_DIRT: {                          /* brown with down-track ruts */
            int rut = (x % 20);
            idx = (rut < 3) ? 2 + (int)((rng >> 16) % 3)
                            : 4 + (int)((rng >> 16) % 10);
            break; }
        default:                                 /* gravel speckle */
            idx = (int)((rng >> 16) % 14);
            break;
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Page 4: crash barrier -- galvanised steel with a dark shadow gutter along the
 * bottom and a rhythm of darker post marks.
 *
 * Reusing the building-wall page (the cheap option) was tried on paper and
 * rejected: that page is banded masonry WITH LIT WINDOWS, so a barrier drawn
 * with it reads as a low garden wall rather than as a road barrier. A page is
 * ~30 lines here, so a dedicated one is the cheaper mistake to avoid.
 *
 * The V axis runs up the barrier's face (see tg_emit_guardrail), so row 0 is
 * the bottom edge -- hence the gutter lives in the low rows, not the high ones.
 */
static void tg_emit_texture_page_rail(TG_Buf *out)
{
    unsigned int rng = 0x7F4A7C15u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..9 steel greys (cooler + brighter than the wall page's concrete),
     * 10..12 shadow/gutter, 13..15 specular highlight along the top rib. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 10)      { b = 150 + i * 8; g = 148 + i * 8; r = 142 + i * 8; }
        else if (i < 13) { b = 60;          g = 58;          r = 56;          }
        else             { b = 228 + (i-13) * 8; g = 228 + (i-13) * 8;
                           r = 224 + (i-13) * 8; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 8) {
            idx = 10 + (int)((rng >> 16) % 3);        /* gutter under the rail */
        } else if (y >= 26 && y <= 34) {
            idx = 13 + (int)((rng >> 17) % 3);        /* highlight rib */
        } else if ((x % 32) < 3) {
            idx = 10 + (int)((rng >> 18) % 3);        /* post every half tile */
        } else {
            idx = (int)((rng >> 16) % 10);            /* steel */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Page 5: bare GROUND -- flat concrete/gravel for the terrain skirt. The point
 * is that it has NO structure: the CITY/INDUSTRIAL biomes used to floor their
 * ground with the WALL page, whose storey/window grid, stretched over the
 * undulating skirt, warped into a rippled "distorted" look filling the
 * background. Just tonal grain here -- a couple of darker gravel patches so it
 * is not a flat sheet, but no lines, so it reads as ground over any slope. */
static void tg_emit_texture_page_ground(TG_Buf *out)
{
    unsigned int rng = 0x2545F491u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..11 mid concrete greys (a touch warmer/darker than the steel rail),
     * 12..15 darker gravel/stain patches. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (104 + i * 5) : (70 - (i - 12) * 8);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)(v > 4 ? v - 4 : 0));   /* faintly warm */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        /* Low-frequency patch mask so darker gravel clumps instead of speckling;
         * no axis-aligned lines, so nothing to shear over a slope. */
        unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 9) * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((patch >> 29) == 0)
            idx = 12 + (int)((rng >> 16) % 4);           /* gravel/stain */
        else
            idx = (int)((rng >> 16) % 12);               /* concrete grain */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R3 BLOCK] pages for parks & houses (feedback items 5-6). One generator,
 * `which` selects the artwork so the four block pages share one function:
 *   0 park LAWN   -- mowed grass, brighter and more uniform than the mottled
 *                    hedgerow GREEN page, with faint mow stripes down-track so a
 *                    flat lawn is not a dead sheet.
 *   1 park HEDGE  -- dense dark clipped foliage, low-frequency clumps, no lines.
 *   2 house WALL  -- warm plaster/brick with small windows and a door, a domestic
 *                    frontage rather than the tall CITY office grid.
 *   3 house ROOF  -- terracotta tiles in horizontal courses.
 * Isotropic where it can be (lawn/hedge tile over a curved skirt); the house
 * pages map one cell per face so a light window grid is fine. */
static void tg_emit_texture_page_r3_block(TG_Buf *out, int which)
{
    unsigned int rng = 0x6C078965u + (unsigned)which * 0x9E3779B9u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* Palette (BGR). */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (which == 0) {            /* mowed grass: green ramp, light->deep */
            b = 40 + i * 3;  g = 96 + i * 8;  r = 44 + i * 4;
        } else if (which == 1) {     /* hedge: darker, denser green */
            b = 24 + i * 2;  g = 58 + i * 6;  r = 22 + i * 3;
        } else if (which == 2) {     /* house wall: warm plaster/brick */
            if (i < 10)      { b = 120 + i * 6; g = 138 + i * 6; r = 158 + i * 6; }
            else if (i < 13) { b = 40;          g = 40;          r = 44;          }
            else             { b = 150 + (i-13)*24; g = 172 + (i-13)*22;
                               r = 196 + (i-13)*18; }   /* lit panes */
        } else {                     /* roof: terracotta tiles */
            if (i < 12) { b = 40 + i * 3; g = 60 + i * 4; r = 120 + i * 8; }
            else        { b = 30;         g = 44;         r = 90 + (i-12)*6; }
        }
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (which == 0) {
            /* Grass grain with a faint vertical mow stripe every 8 texels. */
            int base = 6 + (int)((rng >> 16) % 8);
            if (((x >> 3) & 1) == 0) base += 2;
            if (base > 15) base = 15;
            idx = base;
        } else if (which == 1) {
            /* Clumped foliage, no structure -- reads as a hedge over any slope. */
            unsigned int cell = (unsigned)((y / 8) * 8 + (x / 8)) * 2654435761u;
            idx = ((cell >> 29) == 0) ? (int)((rng >> 16) % 5)
                                      : 5 + (int)((rng >> 16) % 10);
        } else if (which == 2) {
            /* Two window bays + a central door on the ground row. */
            int cell = TD5_TG_TEX_DIM / 3;              /* 3 bays across */
            int wx = x % cell, wy = y % 20;
            if (wy < 2 || wx < 2) {
                idx = 10 + (int)((rng >> 16) % 3);      /* trim / mortar lines */
            } else if (y > 48 && x > 27 && x < 37) {
                idx = 10 + (int)((rng >> 16) % 3);      /* door */
            } else if (wx >= 3 && wx <= cell - 3 && wy >= 5 && wy <= 15) {
                unsigned int pane = (unsigned)((y / 20) * 3 + x / cell)
                                    * 2654435761u;
                idx = ((pane >> 29) == 0) ? (13 + (int)((rng >> 18) % 3)) : 11;
            } else {
                idx = (int)((rng >> 16) % 10);          /* plaster */
            }
        } else {
            /* Terracotta courses: a darker mortar line every 8 rows. */
            idx = ((y & 7) == 0) ? 12 + (int)((rng >> 16) % 4)
                                 : (int)((rng >> 16) % 12);
        }
        tg_put_u8(out, (unsigned)idx);
    }
}


/* ===================== [FB] FEEDBACK-BATCH PAGES =====================
 * One emitter per page slot reserved in the TD5_TG_PAGE_* block above. Each was
 * seeded as tonal grain when the slots were carved and then filled in with real
 * artwork by the area that owns it, so the shared placeholder helper is gone.
 *
 * The recurring lesson in these emitters: a page has to read at the SIZE its
 * geometry maps it at, and several of them are stretched over surfaces that
 * curve or climb, where any axis-aligned feature ladders or shears. */

/* City street furniture: 0 = SIDEWALK paving, 1 = CROSSING (zebra), 2 = FENCE
 * railing (alpha-keyed, index 0 must stay transparent).
 *
 * All three have to read at the SIZE the geometry maps them at, which is what
 * decides the patterns below:
 *   SIDEWALK is mapped isotropically at one page per SPAN_LENGTH (1500 raw), so
 *     16-texel slabs come out at ~375 raw -- a paving slab, not a tile floor.
 *   CROSSING is mapped u = 0..lanes across the road, so each page repeat covers
 *     one lane; two bars per page gives the ~8 bars a 4-lane crossing wants.
 *     Its background must MATCH the asphalt or the crossing reads as a grey
 *     patch on the road rather than paint on it.
 *   FENCE is one page per span, so 8 uprights per page is one every ~190 raw.
 *     Everything that is not metal is index 0 and cuts out. */
static void tg_emit_texture_page_fb_city(TG_Buf *out, int which)
{
    unsigned int rng = 0xC17A0000u + (unsigned)which * 0x9E3779B9u;
    const int type = (which == 2) ? 1 : 0;      /* FENCE is alpha-keyed */
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, (unsigned)type);
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. Index 0 is the transparent key on the FENCE page only -- the other
     * two are opaque, so index 0 is an ordinary colour there. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (which == 2) {
            if (i == 0)      { b = 255; g = 0;   r = 255; }        /* key   */
            else if (i < 11) { b = 118 + i * 5; g = 122 + i * 5; r = 128 + i * 5; }
            else             { b = 62 + i * 2;  g = 64 + i * 2;  r = 68 + i * 2;  }
        } else if (which == 1) {
            /* 0..7 asphalt greys, 8..15 worn white paint. */
            if (i < 8) { b = 52 + i * 4;       g = 52 + i * 4;       r = 54 + i * 4; }
            else       { b = 196 + (i - 8) * 7; g = 198 + (i - 8) * 7; r = 200 + (i - 8) * 7; }
        } else {
            /* 0..11 paving greys, 12..15 darker mortar joints. */
            if (i < 12) { b = 136 + i * 5; g = 138 + i * 5; r = 136 + i * 5; }
            else        { b = 94 - (i - 12) * 7; g = 96 - (i - 12) * 7; r = 94 - (i - 12) * 7; }
        }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;       /* y = 0 is the TOP of the page */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (which == 2) {
            /* Uprights every 8 texels, plus a top rail, a waist rail and a
             * bottom rail. The geometry maps v = 1 at the base, matching this
             * top-down row order. */
            if ((x & 7) < 2 || y < 4 || (y >= 27 && y < 31) || y >= 60)
                idx = 1 + (int)((rng >> 16) % 10);
            else
                idx = 0;                        /* keyed out */
        } else if (which == 1) {
            /* Two full-length bars per page, bars varying in x (== u, across
             * the road) so they run ALONG the direction of travel. Painted
             * edges get a texel of grain so they are not razor-straight. */
            const int b0 = x & 31;
            idx = (b0 >= 3 && b0 < 21) ? (8 + (int)((rng >> 16) % 8))
                                       : (int)((rng >> 16) % 8);
        } else {
            /* 16-texel slabs with mortar joints, plus per-texel grain. */
            idx = ((x & 15) == 0 || (y & 15) == 0)
                  ? 12 + (int)((rng >> 18) % 4)
                  : (int)((rng >> 16) % 12);
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R4 CROSS item 9] CROSS-STREET asphalt with a LONGITUDINAL centre line.
 *
 * The side-street carriageway (tg_city_emit_crossstreet) used the biome road
 * page, whose lane paint is a stripe at the u=0 tile edge -- authored for the
 * MAIN road where u runs 0..lanes ACROSS. On the cross-street the mesh maps u
 * along the OUTWARD run and v across the ~one-lane width, so that same stripe
 * comes out as a rung every ~1500 raw ACROSS the side street -- rungs, not lane
 * markings. The user asked for "lane markers on the perpendicular direction if
 * it's a crossing", i.e. a line running DOWN the side street.
 *
 * This page puts the marking where the cross-street mapping needs it: a dashed
 * white line along the page's X axis (== u == outward) at its Y centre (== v
 * centre == the middle of the street width), so it reads as one broken centre
 * line the length of the perpendicular street. Asphalt grey elsewhere so it
 * still matches the main carriageway it leaves. OPAQUE. */
static void tg_emit_texture_page_r4_cross(TG_Buf *out)
{
    unsigned int rng = 0x5C0554E1u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR: 0..11 asphalt greys (match the main road page), 12..15 near-white. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (44 + i * 3) : (196 + (i - 12) * 12);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;       /* == u, along the street       */
        const int y = i / TD5_TG_TEX_DIM;       /* == v, across the street      */
        int idx;
        rng = rng * 1103515245u + 12345u;
        /* Centre line: 2 rows either side of y=32, dashed along x so it reads as
         * road paint rather than a solid rail. Tiles once in v per span, so the
         * line lands mid-street on every span it covers. */
        if (y >= 30 && y <= 33 && (x % 20) < 12)
            idx = 12 + (int)((rng >> 16) % 4);
        else
            idx = (int)((rng >> 16) % 12);       /* asphalt grain */
        tg_put_u8(out, (unsigned)idx);
    }
}

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

/* First row of the densest TG_TREELINE_WIN-row window of a 64x64 index page. */
static int tg_treeline_src_window(const unsigned char *idx)
{
    int y, best_y = 0, best = -1;

    for (y = 0; y + TG_TREELINE_WIN <= TD5_TG_TEX_DIM; y++) {
        int fill = 0, r, x;
        for (r = y; r < y + TG_TREELINE_WIN; r++)
            for (x = 0; x < TD5_TG_TEX_DIM; x++)
                if (idx[r * TD5_TG_TEX_DIM + x]) fill++;
        if (fill > best) { best = fill; best_y = y; }
    }
    return best_y;
}

/* Most common non-key index inside that window -- the canopy's dominant colour,
 * used whenever a sample lands in a keyed gap and the band must stay solid. */
static int tg_treeline_src_fill(const unsigned char *idx, int y0)
{
    int hist[256], i, r, x, best = 0;

    memset(hist, 0, sizeof(hist));
    for (r = y0; r < y0 + TG_TREELINE_WIN; r++)
        for (x = 0; x < TD5_TG_TEX_DIM; x++) {
            const int v = idx[r * TD5_TG_TEX_DIM + x];
            if (v) hist[v]++;
        }
    for (i = 1; i < 256; i++) if (hist[i] > hist[best]) best = i;
    return best ? best : 1;
}

/* Real-texture band: shipped palette verbatim (no haze tint -- that is exactly
 * what greyed the synthetic page out) over shipped canopy texels, cut off at
 * the top by the same lumpy crown line the synthetic page uses. The crown line
 * is a MASK, not art, so it carries no placeholder colour of its own. */
static void tg_emit_texture_page_fb_treeline_real(TG_Buf *out)
{
    const unsigned char *sidx = k_real_tree_idx[TG_TREELINE_SRC];
    const unsigned char *spal = k_real_tree_pal[TG_TREELINE_SRC];
    const int paln = k_real_tree_paln[TG_TREELINE_SRC];
    const int wy   = tg_treeline_src_window(sidx);
    const int fill = tg_treeline_src_fill(sidx, wy);
    unsigned int rng = 0x77E1B3C5u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 1);                                  /* 1 = alpha-keyed */
    tg_put_u32(out, (unsigned)paln);
    for (i = 0; i < paln * 3; i++) tg_put_u8(out, spal[i]);

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;      /* y=0 is the TOP of the page */
        /* Crown line: 9-texel cells, each a crown of its own height, rounded
         * off at its shoulders, so the top edge is lumpy like a real canopy
         * instead of a straight cut. Keyed above it, foliage below. */
        const unsigned int c = (unsigned)(x / 9) * 2654435761u;
        const int crown = 12 + (int)((c >> 28) % 10);          /* 12..21 */
        const int top   = crown + (((x % 9) < 2 || (x % 9) > 6) ? 3 : 0);
        int cut, span, sx, sy, v, t;

        rng = rng * 1103515245u + 12345u;
        cut = top - (int)((rng >> 22) % 3);                    /* 10..24 */
        if (y < cut) { tg_put_u8(out, 0); continue; }

        /* Nine contiguous source columns per crown cell, each cell starting
         * somewhere else across the page, so the band does not repeat one
         * tree's silhouette along the whole horizon. Vertically the band's
         * crown maps to the window's crown, keeping the source's own top-lit
         * gradient instead of inventing one. */
        span = TD5_TG_TEX_DIM - 1 - cut;
        if (span < 1) span = 1;
        sx = (int)(((c >> 8) + (unsigned)(x % 9)) % (unsigned)TD5_TG_TEX_DIM);
        sy = wy + ((y - cut) * (TG_TREELINE_WIN - 1)) / span;

        /* A sample can land in a keyed gap between the source tree's leaves;
         * step across the window until it finds canopy, and fall back to the
         * dominant colour rather than punching a hole in the backdrop. */
        v = 0;
        for (t = 0; t < 8 && v == 0; t++) {
            const int rx = (sx + t * 5) & (TD5_TG_TEX_DIM - 1);
            const int ry = wy + ((sy - wy + t) % TG_TREELINE_WIN);
            v = sidx[ry * TD5_TG_TEX_DIM + rx];
        }
        tg_put_u8(out, (unsigned)(v ? v : fill));
    }
}

/* Synthetic fallback, used only with TD5RE_AUTOTRACK_REAL_TEX=0. Kept as it
 * was reported so the =0 side stays a faithful "before" to compare against. */
static void tg_emit_texture_page_fb_treeline_proc(TG_Buf *out)
{
    unsigned int rng = 0x77E1B3C5u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 1);                                  /* 1 = alpha-keyed */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0 = key, 1..7 shadowed mass, 8..15 sunlit crowns. A distant
     * treeline is desaturated and blue-shifted by haze, which is what keeps it
     * reading as BACKGROUND rather than a second row of trees. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i == 0)     { b = 255; g = 0;          r = 255; }
        else if (i < 8) { b = 64 + i * 4; g = 62 + i * 5; r = 44 + i * 3; }
        else            { b = 88 + i * 2; g = 92 + i * 4; r = 66 + i * 3; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;      /* y=0 is the TOP of the page */
        /* Crown line: 9-texel cells, each a crown of its own height, rounded
         * off at its shoulders, so the top edge is lumpy like a real canopy
         * instead of a straight cut. Keyed above it, foliage below. */
        const unsigned int c = (unsigned)(x / 9) * 2654435761u;
        const int crown = 12 + (int)((c >> 28) % 10);          /* 12..21 */
        const int top   = crown + (((x % 9) < 2 || (x % 9) > 6) ? 3 : 0);
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < top - (int)((rng >> 22) % 3)) { tg_put_u8(out, 0); continue; }
        /* Lit at the crowns where the sun hits, darker down in the mass. */
        idx = (y < top + 8) ? (8 + (int)((rng >> 16) % 8))
                            : (1 + (int)((rng >> 16) % 7));
        tg_put_u8(out, (unsigned)idx);
    }
}

static void tg_emit_texture_page_fb_treeline(TG_Buf *out)
{
    if (tg_real_textures_enabled()) tg_emit_texture_page_fb_treeline_real(out);
    else                            tg_emit_texture_page_fb_treeline_proc(out);
}

/* [R4 item 5] Distant CITY skyline. Same role and format as the treeline page
 * (alpha-keyed, keyed above the roofline) but the silhouette is blocky building
 * tops rather than a forest crown, so an urban biome's far ridge reads as a city
 * skyline. Hazy cool-grey so it sits BACK like the treeline does, not as a wall
 * of near buildings. Tiles once per span along the band, exactly as the treeline
 * page does, so it shares that page's proven horizontal behaviour. */
static void tg_emit_texture_page_fb_skyline(TG_Buf *out)
{
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 1);                                  /* 1 = alpha-keyed */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0 = key; 1..7 hazy building bodies (blue a touch high, low
     * saturation, so the skyline recedes); 8..15 brighter lit windows / sunlit
     * walls. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i == 0)     { b = 255; g = 0;          r = 255; }
        else if (i < 8) { b = 96 + i * 4; g = 92 + i * 3; r = 86 + i * 3; }
        else            { b = 150 + (i - 8) * 6; g = 148 + (i - 8) * 6;
                          r = 138 + (i - 8) * 5; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;      /* y=0 is the TOP of the page */
        /* ~8-column building blocks, each a flat roof at its own height, so the
         * skyline is a stepped run of low-rise and towers. Keyed above the roof,
         * facade below. */
        const unsigned int c = (unsigned)(x / 8) * 2654435761u;
        int roof = 8 + (int)((c >> 27) % 32);              /* 8..39 rows down */
        int body, win;
        if (((c >> 20) & 7u) == 0u) roof = 3 + (int)((c >> 23) % 5); /* tower */
        if (y < roof) { tg_put_u8(out, 0); continue; }     /* sky above roof */
        /* Facade: a window grid on the lit tones over a per-block body grey.
         * Windows every 4th column (offset per block) and every 4th row down. */
        body = 2 + (int)((c >> 8) % 5);                    /* 2..6 body grey */
        win  = (((unsigned)x & 3u) == ((c >> 4) & 3u)) &&
               (((y - roof) & 3) == 1);
        tg_put_u8(out, (unsigned)(win ? (9 + (int)((c >> 12) % 6)) : body));
    }
}

/* Tunnel lining -- deliberately NOT a building facade: no windows, no storey
 * grid, no straight bright lines at all.
 *
 * Two facts force that. First, the lining is drawn by tg_emit_box_mesh, which
 * sets UV = 2*half/tile and so TILES the page over a face several thousand
 * world units long: a page with any strong axis-aligned feature repeats it as a
 * visible ladder, and shears it the moment the box follows a curve or a grade.
 * Second, the page it used to borrow (TD5_TG_PAGE_WALL) is the city facade --
 * under TD5RE_AUTOTRACK_REAL_TEX a photographic office frontage -- which is why
 * tunnel interiors read as building windows.
 *
 * So: damp cast concrete. A narrow cool-grey ramp plus a LOW-FREQUENCY patch
 * mask (the trick the rail page already uses) so the darker damp stains CLUMP
 * into blotches instead of speckling per texel. Every feature is isotropic, so
 * it tiles and stretches with nothing for the eye to lock onto. */
static void tg_emit_texture_page_fb_tunnel(TG_Buf *out)
{
    unsigned int rng = 0x7011u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..10 cool concrete greys (blue channel a touch high, so the lining
     * stays distinct from the warm-grey ground page); 11..15 darker damp/soot
     * stains for the patch mask below. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 11) ? (92 + i * 4) : (66 - (i - 11) * 9);
        if (v < 0) v = 0;
        tg_put_u8(out, (unsigned)(v + 6 < 255 ? v + 6 : 255));  /* B, cooler */
        tg_put_u8(out, (unsigned)v);                            /* G */
        tg_put_u8(out, (unsigned)v);                            /* R */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        /* 8x8-texel cells hashed to a patch id: blotches ~1/8 of the page wide,
         * which at the 3000-unit tile the walls use is a stain a car length
         * across. No axis-aligned run survives, so nothing shears. */
        unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 11) * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((patch >> 29) < 2)
            idx = 11 + (int)((rng >> 16) % 5);          /* damp / soot stain */
        else
            idx = (int)((rng >> 16) % 11);              /* concrete grain */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Extra tunnel LININGS for item 16a: same isotropic cast-concrete recipe as the
 * base page above (no axis-aligned feature, so nothing shears as a bore curves),
 * but each variant shifts the grey ramp and its warmth and its stain density so
 * consecutive tunnels plainly differ. variant is 1..4; variant 0 is the base
 * page and is not generated here. The palette/texel structure is identical to
 * tg_emit_texture_page_fb_tunnel so the two pages sit at the same tone family --
 * a tunnel network, not four unrelated boxes. */
static void tg_emit_texture_page_tunnel_var(TG_Buf *out, int variant)
{
    /* vi indexes the tweak tables. Per variant: grey-ramp start, R/G/B tint
     * offsets (warm sand / cool soot / pale new / damp green-blue) and a stain
     * frequency (higher = more damp blotching). */
    static const int base_lo[4]  = { 100,  74, 122,  90 };
    static const int dR[4]       = {  16,   0,   4, -10 };
    static const int dG[4]       = {   4,  -2,   2,   6 };
    static const int dB[4]       = { -10,   6,   0,  14 };
    static const unsigned bl[4]  = {   2u,  3u,  1u,  2u };
    const int vi = (variant >= 1 && variant <= 4) ? variant - 1 : 0;
    unsigned int rng = 0x7011u + (unsigned)variant * 0x9E3779B1u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 11) ? (base_lo[vi] + i * 4)
                         : (base_lo[vi] - 30 - (i - 11) * 9);
        int b = v + dB[vi], g = v + dG[vi], r = v + dR[vi];
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 11) * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((patch >> 29) < bl[vi])
            idx = 11 + (int)((rng >> 16) % 5);          /* damp / soot stain */
        else
            idx = (int)((rng >> 16) % 11);              /* concrete grain */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R6 TUNNEL item 8c] Proper cast-concrete bore lining, replacing the isotropic
 * grain that read in frame as a grey checkerboard placeholder. A real road-tunnel
 * lining has a clear ROAD-LEVEL DATUM -- a dark dado/kerb band at the bottom with
 * a bright reflective strip just above it -- and light concrete PANELS divided by
 * thin recessed joints above that. The datum keys on the V axis, which in the
 * swept mesh is HEIGHT up the wall (row y=0 = v=0 = road level), so it stays put
 * as the bore curves; the panel joints run both axes but stay low-contrast so a
 * curving segment cannot shear a hard line. variant 0..3 shift tone/warmth and
 * panel pitch so consecutive bores plainly differ (item 16a lives on here). */
static void tg_emit_texture_page_r6_tunnel_lining(TG_Buf *out, int variant)
{
    static const int base_lo[4] = { 150, 134, 168, 122 }; /* light-concrete floor */
    static const int warm[4]    = {  10,  -6,   2,  -3 };  /* +R/-B = warm, -R/+B = cool */
    static const int pitch[4]   = {  16,  16,  21,  13 };  /* panel size, texels */
    const int vi = (variant >= 0 && variant <= 3) ? variant : 0;
    const int lo = base_lo[vi];
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* 0..9 light concrete ramp; 10 bright reflective strip; 11 recessed joint;
     * 12..13 dark dado/kerb; 14..15 mid soot stain. BGR, R vs B tilt = warmth. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v, b, g, r;
        if      (i <= 9)  v = lo + i * 3;
        else if (i == 10) v = 236;
        else if (i == 11) v = lo - 58;
        else if (i <= 13) v = lo - 80;
        else              v = lo - 34;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        b = v - warm[vi]; g = v; r = v + warm[vi];
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        const int p = pitch[vi];
        int idx;
        if (y <= 8)                             /* road-level dado band */
            idx = 12 + ((x ^ y) & 1);
        else if (y <= 10)                       /* reflective strip above dado */
            idx = 10;
        else if ((x % p) == 0 || (y % p) == 0)  /* recessed panel joints */
            idx = 11;
        else {                                  /* concrete panels, faint variation */
            unsigned int h = (unsigned)((x / p) + (y / p) * 7) * 2654435761u;
            idx = (int)((h >> 28) % 8);
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R6 TUNNEL item 8b] Portal header for the run mouths. The lintel samples only
 * v 0..0.17 (a short band above the opening), so the design lives in the low
 * rows: a bright reflective lower lip that meets the mouth top, then a lit
 * concrete face. Reads as a proper portal beam instead of the washed-out white
 * lining the swept mouth used before. */
static void tg_emit_texture_page_r6_tunnel_portal(TG_Buf *out)
{
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* 0..11 warm-grey concrete ramp; 12 bright reflective lip; 13..15 dark trim. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v, b, g, r;
        if      (i <= 11) v = 120 + i * 8;
        else if (i == 12) v = 242;
        else              v = 70 - (i - 13) * 16;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        b = v - 8; g = v; r = v + 8;             /* faintly warm */
        b = b < 0 ? 0 : b;
        r = r > 255 ? 255 : r;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        int idx;
        if (y <= 2)                     idx = 13 + (y % 3);   /* dark shadow lip top */
        else if (y <= 5)                idx = 12;             /* bright reflective lip */
        else if ((x % 32) < 1)          idx = 13;             /* pier joint */
        else                            idx = 5 + ((x >> 4) & 3) + ((y >> 4) & 1);
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R6 TUNNEL item 8d] Emissive wall-lamp strip. The engine has no interior
 * lighting, so a "lamp" is a bright textured quad drawn with a warm-white vertex
 * colour: a hot core tube with a small dark housing top and bottom. Tiled along
 * the bore top on both walls it gives the run a run of side lights. */
static void tg_emit_texture_page_r6_tunnel_lamp(TG_Buf *out)
{
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* 0 dark housing; 1..3 warm halo ramp; 4..15 hot white-yellow core. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i == 0)      { b = 24;  g = 22;  r = 20; }        /* housing */
        else if (i <= 3) { b = 120 + i * 20; g = 150 + i * 24; r = 170 + i * 26; }
        else             { b = 210; g = 244; r = 255; }       /* hot core */
        b = b > 255 ? 255 : b; g = g > 255 ? 255 : g; r = r > 255 ? 255 : r;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int y = i / TD5_TG_TEX_DIM;
        int idx;
        if (y <= 6 || y >= 57)      idx = 0;                  /* housing top/bottom */
        else if (y <= 12 || y >= 51) idx = 1 + ((y ^ i) & 2); /* warm halo */
        else                        idx = 4 + (i & 11);       /* hot core */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Bridge DECK surface (item 11). The road quad maps this page v = si + f, one
 * page repeat per span, u = 0..lanes across -- so a page with any longitudinal
 * (lane-paint) feature would tile down the deck as a stack of identical road
 * tiles, which is exactly the "tiled road on a bridge" report. Two rules follow:
 *   - ACROSS the deck (u): plain cast concrete, NO lane markings.
 *   - ALONG the deck (v): one dark EXPANSION-JOINT band per page repeat, i.e.
 *     one transverse seam per span, which is what a real segmented deck shows
 *     and reads as structure rather than tarmac. The seam is transverse, so it
 *     only stays straight because bridge runs are kept near-straight (item 15). */
static void tg_emit_texture_page_bridge_deck(TG_Buf *out)
{
    unsigned int rng = 0xBD9C5A11u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..12 neutral mid concrete greys, 13..15 dark joint/shadow. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 13) ? (120 + i * 4) : (58 - (i - 13) * 12);
        if (v < 0) v = 0;
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        /* The expansion joint: the bottom 3 rows (v just past the span join) go
         * dark, plus a 1-row soft shadow. A darker slab per ~8 texels elsewhere
         * so the concrete is not a flat sheet. No vertical (lane) structure. */
        unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 9) * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 3)              idx = 13 + (int)((rng >> 16) % 3);  /* joint    */
        else if (y == 3)        idx = 12;                          /* shadow   */
        else if ((patch >> 30) == 0)
                                idx = 8 + (int)((rng >> 16) % 4);  /* stain    */
        else                    idx = (int)((rng >> 16) % 12);     /* concrete */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R4 item 16b] GUARDRAIL face for the sloped bridge parapet. Its quad maps
 * u across the panel HEIGHT and v along the road, so this page is authored in
 * those axes (unlike TD5_TG_PAGE_RAIL, an armco meant to lie horizontally, which
 * sampled rotated here and read as noise). page-X therefore IS the barrier
 * height (0 = deck, 63 = top) and page-Y runs along the road:
 *   - a solid pale-concrete barrier body,
 *   - a darker METAL CAP RAIL across the top rows (constant height),
 *   - a shadow groove at mid height,
 *   - vertical expansion JOINTS at a road interval (bands in page-Y). */
static void tg_emit_texture_page_r4_guardrail(TG_Buf *out)
{
    unsigned int rng = 0x51E7A113u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..3 dark steel (cap rail / groove), 4..11 pale concrete body,
     * 12..15 sunlit concrete highlight. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 4)       { b = 96 + i * 6;  g = 98 + i * 6;  r = 100 + i * 6; }
        else if (i < 12) { b = 168 + (i-4) * 6; g = 170 + (i-4) * 6;
                           r = 172 + (i-4) * 6; }
        else             { b = 224 + (i-12) * 6; g = 226 + (i-12) * 6;
                           r = 228 + (i-12) * 6; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;   /* barrier HEIGHT, 0=deck..63=top */
        const int y = i / TD5_TG_TEX_DIM;   /* along the road                 */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((y % 22) < 2)          idx = 1 + (int)((rng >> 16) % 3); /* joint  */
        else if (x >= 55)          idx = (int)((rng >> 16) % 4);     /* cap    */
        else if (x >= 50)          idx = 12 + (int)((rng >> 16) % 4);/* cap lip*/
        else if (x >= 30 && x <= 32) idx = 1;                        /* groove */
        else                       idx = 4 + (int)((rng >> 16) % 8); /* body   */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R4 item 18] PIER / TOWER concrete. Smooth cast concrete for a LIT exterior:
 * a tight pale-grey ramp with faint vertical form-board seams and only sparse
 * grain -- deliberately none of the damp/soot blotching the bore LINING page
 * carries, which read as a checkerboard on a bright tower. */
static void tg_emit_texture_page_r4_pier(TG_Buf *out)
{
    unsigned int rng = 0x9A31C7E5u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..11 a narrow pale-concrete ramp, 12..15 faint form-seam shadow. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (176 + i * 5) : (150 - (i - 12) * 10);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)(v > 3 ? v - 3 : 0));   /* faintly warm grey */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        unsigned int patch = (unsigned)((x >> 4) + (y >> 4) * 5) * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((x % 16) == 0)          idx = 12 + (int)((rng >> 16) % 3); /* seam */
        else if ((patch >> 30) == 0) idx = 6 + (int)((rng >> 16) % 3); /* grain*/
        else                        idx = (int)((rng >> 16) % 12);     /* face */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R4 item 20] COASTLINE / shore band where the bridge water meets the bank.
 * Warm sand grading into darker wet sand / shingle at the waterline. Isotropic
 * and line-free like the GROUND page, so it never shears where the shore slopes;
 * the wet band is a low-frequency patch, not an axis-aligned edge. */
static void tg_emit_texture_page_r4_coast(TG_Buf *out)
{
    unsigned int rng = 0xC0A57011u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR, warm: r > g > b. 0..10 dry sand ramp, 11..15 darker wet sand/shingle. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int base = (i < 11) ? (150 + i * 8) : (120 - (i - 11) * 12);
        if (base < 0) base = 0;
        if (base > 255) base = 255;
        tg_put_u8(out, (unsigned)(base > 40 ? base - 40 : 0));   /* b */
        tg_put_u8(out, (unsigned)(base > 18 ? base - 18 : 0));   /* g */
        tg_put_u8(out, (unsigned)base);                          /* r */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 7) * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((patch >> 29) < 2u)
            idx = 11 + (int)((rng >> 16) % 5);           /* wet sand / shingle */
        else
            idx = (int)((rng >> 16) % 11);               /* dry sand grain     */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R5 item 17a] ARMCO steel guardrail face. The R4 page reads as a concrete
 * barrier ("still not a guardrail texture"); this is the galvanized W-beam the
 * user expects: cool blue-grey steel with the two horizontal corrugation ridges
 * of a W-section, a dark rail cap, and vertical posts at a road interval.
 *
 * Same axes as the R4 page so the panel UVs need no change: page-X is the
 * barrier HEIGHT (0 = deck .. 63 = top), page-Y runs ALONG the road. The two
 * beam ridges are therefore constant-X bands and the posts are constant-Y bands. */
static void tg_emit_texture_page_r5_guardrail(TG_Buf *out)
{
    unsigned int rng = 0x7A5CE001u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR galvanized steel: cool (b >= g >= r). 0..3 dark groove/post shadow,
     * 4..11 mid steel ramp, 12..15 bright spangle highlight. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v;
        if (i < 4)       v = 70 + i * 10;            /* dark groove / shadow */
        else if (i < 12) v = 130 + (i - 4) * 9;      /* mid steel            */
        else             v = 210 + (i - 12) * 11;    /* bright spangle       */
        if (v > 255) v = 255;
        tg_put_u8(out, (unsigned)v);                             /* b (coolest) */
        tg_put_u8(out, (unsigned)(v > 6 ? v - 6 : 0));          /* g           */
        tg_put_u8(out, (unsigned)(v > 12 ? v - 12 : 0));        /* r           */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;   /* barrier HEIGHT, 0=deck..63=top */
        const int y = i / TD5_TG_TEX_DIM;   /* along the road                 */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((y % 20) < 3)                    /* vertical post, full height     */
            idx = 1 + (int)((rng >> 16) % 2);
        else if (x >= 58)                    /* dark rail cap along the top    */
            idx = (int)((rng >> 16) % 2);
        else if ((x >= 22 && x <= 26) || (x >= 40 && x <= 44))
            idx = 12 + (int)((rng >> 16) % 4);   /* the two W-beam ridge crests */
        else if (x == 33 || x < 8)           /* centre valley + skirt shadow    */
            idx = 1 + (int)((rng >> 16) % 3);
        else                                 /* steel body, faint spangle       */
            idx = 4 + (int)((rng >> 16) % 8);
        tg_put_u8(out, (unsigned)idx);
    }
}

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
static void tg_emit_texture_page_r6_guardrail_alpha(TG_Buf *out)
{
    unsigned int rng = 0x6A1CD00Du;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 1);                                    /* 1 = alpha-keyed */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* index 0 = transparent key (magenta, never drawn); 1..3 dark steel edge/
     * post shadow, 4..11 mid galvanized steel, 12..15 bright spangle. Cool
     * (b >= g >= r) like the R5 armco so lit/shadowed rails still match. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v;
        if (i == 0) { tg_put_u8(out, 255); tg_put_u8(out, 0); tg_put_u8(out, 255); continue; }
        if (i < 4)       v = 74 + i * 10;
        else if (i < 12) v = 132 + (i - 4) * 9;
        else             v = 212 + (i - 12) * 11;
        if (v > 255) v = 255;
        tg_put_u8(out, (unsigned)v);                            /* b (coolest) */
        tg_put_u8(out, (unsigned)(v > 6 ? v - 6 : 0));          /* g           */
        tg_put_u8(out, (unsigned)(v > 12 ? v - 12 : 0));        /* r           */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;   /* barrier HEIGHT, 0=deck..63=top */
        const int y = i / TD5_TG_TEX_DIM;   /* along the road                 */
        const int post = (y % 18) < 3;            /* vertical post at a pitch  */
        const int beam = (x >= 16 && x <= 52);    /* the W-beam's vertical run */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (beam) {
            if ((x >= 20 && x <= 24) || (x >= 44 && x <= 48))
                idx = 12 + (int)((rng >> 16) % 4);   /* the two W ridge crests */
            else if (x == 34 || x == 16 || x == 52)
                idx = 1 + (int)((rng >> 16) % 2);    /* centre valley + edges  */
            else
                idx = 4 + (int)((rng >> 16) % 8);    /* steel body             */
        } else if (post) {
            idx = 1 + (int)((rng >> 16) % 3);        /* post above/below beam  */
        } else {
            idx = 0;                                 /* transparent air        */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R6 item 17] STEEL pier/tower face -- cool blue-grey plate steel with dark
 * I-beam flange shadows down the edges and centre and a rivet grid, so a
 * steel-style crossing reads as a braced viaduct, not cast concrete. Opaque. */
static void tg_emit_texture_page_r6_pier_steel(TG_Buf *out)
{
    unsigned int rng = 0x53EE1002u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 4) ? (60 + i * 10)
              : (i < 12) ? (120 + (i - 4) * 10)
                         : (210 + (i - 12) * 11);
        if (v > 255) v = 255;
        tg_put_u8(out, (unsigned)v);                          /* b (coolest) */
        tg_put_u8(out, (unsigned)(v > 4 ? v - 4 : 0));        /* g           */
        tg_put_u8(out, (unsigned)(v > 10 ? v - 10 : 0));      /* r           */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (x < 6 || x > 57 || (x >= 30 && x <= 33))
            idx = (int)((rng >> 16) % 4);            /* I-beam flange shadow */
        else if ((x % 8) == 0 && (y % 8) == 0)
            idx = 12 + (int)((rng >> 16) % 4);       /* rivet head highlight */
        else
            idx = 4 + (int)((rng >> 16) % 8);        /* web steel            */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R7 item 9] Concrete TUNNEL PORTAL facade. The R6 portal was a single thin
 * lintel beam over the mouth on TD5_TG_PAGE_R6_TUNNEL+4, so the entrance read as
 * "a weird grey texture ... doesn't look like the entrance of a tunnel". Shipped
 * TD5 has no portal-facade sprite to mine: its *TUN*.TGA billboards are interior
 * "light at the end of the tunnel" glows (single/twin arch glows, edge-light
 * streaks) and the mouth surround is darkened track MESH, confirmed by decoding
 * all 12 environs *TUN* pages. So this page is authored to dress a proper facade
 * FRAME (header + jambs, built in tg_emit_tunnel_swept) as cast concrete civil
 * work: a bright coping band along the top, horizontal form-board courses and
 * vertical form-joint lines over a warm-grey field, a weathered darker base.
 * Opaque. y=0 is the TOP of the page (coping), so the frame maps V=0 to its top
 * edge. */
static void tg_emit_texture_page_r7_tunnel_portal(TG_Buf *out)
{
    unsigned int rng = 0x7A11B00Cu;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* 0..1 form-joint shadow, 2..12 concrete ramp, 13 bright coping, 14..15 base. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 2)  ? (56 + i * 20)
              : (i < 13) ? (120 + (i - 2) * 11)
              : (i == 13) ? 238
                          : (90 - (i - 14) * 22);
        int b, g, r;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        b = v - 6; g = v; r = v + 8;                        /* faintly warm */
        if (b < 0)   b = 0;
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;              /* y=0 is the TOP */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 4)                        idx = 13;    /* coping highlight  */
        else if (y == 4 || y == 5)        idx = 1;     /* shadow under coping */
        else if (y > 58)                  idx = 14 + (int)((rng >> 16) & 1); /* base */
        else if ((y % 20) < 1)            idx = 1;     /* horizontal form course */
        else if ((x % 32) < 1)            idx = 2;     /* sparse vertical joint */
        else idx = 4 + (int)(((unsigned)((x >> 4) + (y >> 4)) + (rng >> 19)) % 7u);
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R6 item 17] MASONRY pier face -- warm sandstone ashlar in staggered courses
 * (mortar lines every few rows, vertical joints offset course to course), so a
 * stone-style crossing reads as a masonry viaduct. Opaque. */
static void tg_emit_texture_page_r6_pier_stone(TG_Buf *out)
{
    unsigned int rng = 0x57021ACEu;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* warm sandstone, r >= g >= b. 0..2 mortar shadow, 3..15 stone ramp. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int base = (i < 3) ? (70 + i * 10) : (120 + (i - 3) * 10);
        if (base > 255) base = 255;
        tg_put_u8(out, (unsigned)(base > 30 ? base - 30 : 0));   /* b */
        tg_put_u8(out, (unsigned)(base > 14 ? base - 14 : 0));   /* g */
        tg_put_u8(out, (unsigned)base);                          /* r */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        const int course = y / 10;
        const int joint  = ((x + (course & 1) * 8) % 16) < 1;   /* staggered */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((y % 10) < 1 || joint)
            idx = (int)((rng >> 16) % 3);            /* mortar line */
        else
            idx = 3 + (int)((rng >> 16) % 13);       /* stone face  */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Terrain: 0 = SNOW ground, 1 = distant HILL / mountain flank.
 *
 * Both follow the rule the GROUND page comment sets out: NO structure and no
 * axis-aligned lines, because these pages are stretched over sloping,
 * undulating terrain where any grid shears into a rippled pattern. All the
 * variation is low-frequency patches plus per-texel grain.
 *
 * SNOW is deliberately not pure white: a flat 255 sheet loses all shape at
 * distance and every mip level collapses to the same colour, so the palette runs
 * cool blue-shadow to sunlit white and a coarse patch mask picks out drifts.
 *
 * HILL is mapped v=0 at the CREST (tg_emit_far_band winds the ridge with v=1 at
 * the base), so the page's TOP rows are the snowline and the rest is rock. */
static void tg_emit_texture_page_fb_terrain(TG_Buf *out, int which)
{
    unsigned int rng = which ? 0x4111BEEFu : 0x4222FEEDu;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. SNOW: 0..5 blue-grey shadow, 6..15 up to near-white sunlit crust.
     * HILL: 0..9 rock (cool grey-brown), 10..15 pale snowline. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (!which) {
            if (i < 6) { b = 208 + i * 6; g = 198 + i * 7; r = 188 + i * 8; }
            else       { b = 244 + (i - 6); g = 240 + (i - 6); r = 236 + (i - 6) * 2; }
        } else {
            if (i < 10) { b = 92 + i * 4; g = 96 + i * 4; r = 88 + i * 5; }
            else        { b = 214 + (i - 10) * 8; g = 212 + (i - 10) * 8;
                          r = 208 + (i - 10) * 9; }
        }
        if (b > 255) b = 255;
        if (g > 255) g = 255;
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        /* Coarse 8x8-texel patch mask: drifts on snow, shadowed faces on rock. */
        const unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 11)
                                 * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (!which) {
            idx = 6 + (int)((rng >> 16) % 10);                /* sunlit crust */
            if ((patch >> 30) == 0) idx = (int)((rng >> 18) % 6);  /* drift shade */
        } else {
            /* Snowline over the top ~14 rows, ragged so it is not a hard band. */
            const int line = 10 + (int)((patch >> 29) % 5);
            if (y < line) idx = 10 + (int)((rng >> 16) % 6);
            else          idx = (int)((rng >> 16) % 10);
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Gantry FRAME page: the uprights and the panel's underside cap.
 *
 * [FB r2 item 12] The panel itself no longer samples this page -- it carries the
 * shipped START / FINISH artwork instead (see tg_emit_gantry). Only the legs
 * (u in [0, 0.12], the left column band) and the cap (v in [0.9, 1]) are left
 * here, so the chequer band is now just what the cap's underside shows. Painted
 * rather than grained, because both users map flat unlit strips of it. */
static void tg_emit_texture_page_fb_banner(TG_Buf *out)
{
    /* 0 = post grey, 1 = surround (near black), 2 = white, 3 = mid grey. */
    static const unsigned char k_pal[4][3] = {
        {  92,  92,  94 },   /* BGR: post/leg grey       */
        {  22,  22,  24 },   /* surround                 */
        { 236, 240, 244 },   /* chequer light            */
        {  34,  34,  36 }    /* chequer dark             */
    };
    const int band_lo = TD5_TG_TEX_DIM / 4;          /* chequer band, top    */
    const int band_hi = TD5_TG_TEX_DIM - band_lo;    /* chequer band, bottom */
    const int cell    = TD5_TG_TEX_DIM / 8;          /* 8 units, so 8x4 cells */
    const int leg_col = (TD5_TG_TEX_DIM * 12) / 100; /* the legs' u window   */
    int i, y, x;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                /* opaque page */
    tg_put_u32(out, TD5_TG_PAL_COUNT);
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        const unsigned char *c = k_pal[i & 3];
        tg_put_u8(out, c[0]); tg_put_u8(out, c[1]); tg_put_u8(out, c[2]);
    }

    for (y = 0; y < TD5_TG_TEX_DIM; y++) {
        for (x = 0; x < TD5_TG_TEX_DIM; x++) {
            int idx;
            if (x < leg_col)                       idx = 0;   /* leg column   */
            else if (y < band_lo || y >= band_hi)  idx = 1;    /* surround     */
            else idx = (((x / cell) + (y / cell)) & 1) ? 3 : 2;
            tg_put_u8(out, (unsigned)idx);
        }
    }
}

/* [R5 STRUCT item 1] Dedicated SOLID leg-face page for the start/finish gantry
 * uprights. The legs used to sample a 12% window (u 0..0.12) of the shared
 * BANNER page, whose left column is post-grey and the rest is the black surround
 * + chequer end block. But the solid leg column is only 7 texels wide (leg_col =
 * 64*12/100 = 7, u <= 0.109), so the leg's u=0.12 right edge OVERSHOOTS the
 * column into the surround and chequer -- the reported "texture not fully
 * covering it, bleeding the edges into another texture" (framedump
 * log/r5s_leg_left_base.png: a black strip and white chequer blobs down the leg
 * edge). This page is cast concrete edge to edge, so there is no neighbouring
 * region for the leg UVs to bleed into -- the bleed is impossible by
 * construction, not merely inset away. Opaque, neutral grey (BGR == R == G),
 * matched to the old post-grey (~92) with faint vertical cast-joint lines so a
 * column reads as a column rather than a flat card. */
static void tg_emit_texture_page_r5_leg(TG_Buf *out)
{
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR, neutral grey around the old post-grey (92,92,94). A small spread so
     * the joint lines below have something to pick from. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = 78 + (i % 8) * 5;                            /* 78..113 */
        tg_put_u8(out, (unsigned)v);   /* B */
        tg_put_u8(out, (unsigned)v);   /* G */
        tg_put_u8(out, (unsigned)v);   /* R */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int idx = 3 + ((x * 7 + y) & 1);                     /* base concrete    */
        /* Two faint vertical cast joints so the post has some vertical grain. */
        if (x == TD5_TG_TEX_DIM / 3 || x == (2 * TD5_TG_TEX_DIM) / 3) idx = 1;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R4 BRANCH item 8] Vertical concrete KERB face for the two side walls of an
 * avenue median (see tg_emit_avenue_divider). Plain cast concrete: a lighter
 * kerb cap at the page top (v -> 0 is the top of the wall, matching the tree
 * page's "y=0 is the TOP" convention) over a mid-grey body with faint cast-joint
 * lines, so a planted or kerbed median reads as a real kerb instead of grass or
 * paving slabs climbing its walls. Opaque, neutral grey (BGR == R == G). */
static void tg_emit_texture_page_branch_kerb(TG_Buf *out)
{
    unsigned int rng = 0x4B00B1E5u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR, neutral grey. 0..11 concrete body greys, 12..15 the light kerb cap. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (96 + i * 6) : (188 + (i - 12) * 10);
        tg_put_u8(out, (unsigned)v);   /* B */
        tg_put_u8(out, (unsigned)v);   /* G */
        tg_put_u8(out, (unsigned)v);   /* R */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;    /* y = 0 is the TOP of the page */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 6) idx = 12 + (int)((rng >> 17) % 4);    /* light kerb cap */
        else       idx = (int)((rng >> 16) % 12);        /* concrete grain */
        if (y >= 6 && (y % 20) == 0)                     /* faint cast joints */
            idx = (idx > 2) ? idx - 2 : 0;
        (void)x;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Emit a page from real (already palette-indexed) TD5 texture data, in the same
 * on-disk page format as the procedural emitters -- pad, opaque type, palette
 * count, BGR palette, then the 64x64 index bytes. */
static void tg_emit_real_page(TG_Buf *out, const unsigned char *pal, int paln,
                              const unsigned char *idx, int type)
{
    int i;
    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, (unsigned)type);         /* 0 opaque, 1 alpha-keyed (idx 0) */
    tg_put_u32(out, (unsigned)paln);
    for (i = 0; i < paln * 3; i++) tg_put_u8(out, pal[i]);
    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) tg_put_u8(out, idx[i]);
}

/* Fill the wall/store/grass/tree/prop pages with REAL TD5 texture data borrowed
 * from shipped tracks instead of the procedural placeholders, so the auto-track
 * reads like an actual TD5 level.
 *
 * DEFAULT ON since 2026-08-27 (TD5RE_AUTOTRACK_REAL_TEX=0 restores procedural
 * art). It was opt-in while td5_tg_real_tex.h only carried the facade set, and
 * the header comment above still said "tree + rail stay procedural" long after
 * the foliage pages landed -- which is why placeholder tree silhouettes were
 * still what a default run put on screen. The borrowed set is now complete for
 * every page this branch fills, VERIFIED against the header data:
 *   wall  5 of TD5_TG_WALL_VARIANTS  5   store 3 of TD5_TG_STORE_VARIANTS 3
 *   tree 10 of TD5_TG_TREE_VARIANTS 10   prop  7 of TD5_TG_PROP_COUNT     7
 * and every one of those 25 pages is well formed -- palette length == 3*paln,
 * 4096 index bytes, max index < paln. The 17 alpha-keyed pages (10 tree, 7
 * prop) all carry palette entry 0 = black with real key coverage (tree 35..83%
 * of texels, prop 32..74%), so index 0 is genuinely the transparent key on all
 * of them and nothing keys away real foliage.
 *
 * Nothing ELSE moves when this flips: road, ground, rail, water, the themed
 * road surfaces and every [FB] page are emitted procedurally on BOTH sides of
 * the branch (road/ground deliberately so -- see tg_emit_textures). */
static int tg_real_textures_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_REAL_TEX");
}

/* Street FURNITURE (the kerb railing) on shipped art rather than the painted
 * stand-in. Default ON and independent of TD5RE_AUTOTRACK_REAL_TEX: that knob
 * trades a neutral generic street for photographic Melbourne facades, which is
 * a taste call, whereas a real railing has no such downside. The knob exists so
 * the painted page is still one A/B away. */
static int tg_furniture_real_pages(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_REAL_FURNITURE");
}

static int tg_emit_textures(TG_Buf *out)
{
    TG_Buf pages[TD5_TG_PAGE_COUNT];
    const unsigned int count = TD5_TG_PAGE_COUNT;
    unsigned int cursor = 4 + 4 * count;
    unsigned int i;

    memset(pages, 0, sizeof(pages));

    /* ROAD and GROUND are ALWAYS procedural: the shipped city road/ground pages
     * are sandstone tan and read muddy under the auto-track, whereas the
     * procedural asphalt is grey with lane paint and the procedural ground is
     * neutral grey concrete -- both closer to what a generic street wants. Only
     * the FACADE walls (and grass) borrow real TD5 pages, which is where the
     * photographic detail actually pays off. */
    tg_emit_texture_page_asphalt(&pages[TD5_TG_PAGE_ROAD]);
    tg_emit_texture_page_ground(&pages[TD5_TG_PAGE_GROUND]);
    if (tg_real_textures_enabled()) {
        int v, w;
        tg_emit_real_page(&pages[TD5_TG_PAGE_WALL],
                          k_real_wall_pal[0], k_real_wall_paln[0], k_real_wall_idx[0], 0);
        for (v = 1; v < k_real_wall_count && v < TD5_TG_WALL_VARIANTS; v++)
            tg_emit_real_page(&pages[TD5_TG_PAGE_WALL_EXTRA + v - 1],
                              k_real_wall_pal[v], k_real_wall_paln[v], k_real_wall_idx[v], 0);
        /* Variants from the OTHER city tracks, laid down after the level014
         * ones: low-rise masonry, then the tower class at WALL_TOWER_FIRST.
         * Each loop stops at the smaller of its source count and the slots it
         * owns, so adding a page to either header cannot walk into the next
         * group's pages. */
        for (v = 0; v < k_real_city_low_count; v++) {
            w = k_real_wall_count + v;
            if (w >= TD5_TG_WALL_TOWER_FIRST) break;
            tg_emit_real_page(&pages[TD5_TG_PAGE_WALL_EXTRA + w - 1],
                              k_real_city_low_pal[v], k_real_city_low_paln[v],
                              k_real_city_low_idx[v], 0);
        }
        for (v = 0; v < k_real_city_tower_count; v++) {
            w = TD5_TG_WALL_TOWER_FIRST + v;
            if (w >= TD5_TG_WALL_VARIANTS) break;
            tg_emit_real_page(&pages[TD5_TG_PAGE_WALL_EXTRA + w - 1],
                              k_real_city_tower_pal[v], k_real_city_tower_paln[v],
                              k_real_city_tower_idx[v], 0);
        }
        for (v = 0; v < k_real_store_count && v < TD5_TG_STORE_VARIANTS; v++)
            tg_emit_real_page(&pages[TD5_TG_PAGE_STORE + v],
                              k_real_store_pal[v], k_real_store_paln[v], k_real_store_idx[v], 0);
        for (v = 0; v < k_real_city_store_count; v++) {
            w = k_real_store_count + v;
            if (w >= TD5_TG_STORE_VARIANTS) break;
            tg_emit_real_page(&pages[TD5_TG_PAGE_STORE + w],
                              k_real_city_store_pal[v], k_real_city_store_paln[v],
                              k_real_city_store_idx[v], 0);
        }
        /* [R7 item 4] extra facade variety in the R7_CITY block. Any slot the
         * header does not fill falls back to a procedural wall so the selection
         * pool never lands on a blank (black) page. */
        for (v = 0; v < TD5_TG_R7_WALL_LOW_N; v++) {
            if (v < k_real_r7city_low_count)
                tg_emit_real_page(&pages[TD5_TG_PAGE_R7_WALL_LOW + v],
                                  k_real_r7city_low_pal[v],
                                  k_real_r7city_low_paln[v],
                                  k_real_r7city_low_idx[v], 0);
            else
                tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_R7_WALL_LOW + v],
                                          TD5_TG_WALL_VARIANTS + v);
        }
        for (v = 0; v < TD5_TG_R7_WALL_TOWER_N; v++) {
            if (v < k_real_r7city_tower_count)
                tg_emit_real_page(&pages[TD5_TG_PAGE_R7_WALL_TOWER + v],
                                  k_real_r7city_tower_pal[v],
                                  k_real_r7city_tower_paln[v],
                                  k_real_r7city_tower_idx[v], 0);
            else
                tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_R7_WALL_TOWER + v],
                                          TD5_TG_WALL_VARIANTS
                                          + TD5_TG_R7_WALL_LOW_N + v);
        }
        tg_emit_real_page(&pages[TD5_TG_PAGE_GREEN],
                          k_real_green_pal, k_real_green_paln, k_real_green_idx, 0);
        /* Thematic trees: alpha-keyed (type 1), index 0 transparent. */
        for (v = 0; v < TD5_TG_TREE_VARIANTS && v < k_real_tree_count; v++)
            tg_emit_real_page(&pages[tg_tree_slot(v)],
                              k_real_tree_pal[v], k_real_tree_paln[v], k_real_tree_idx[v], 1);
        /* [R5 item 18] Tall Moscow park trees, alpha-keyed like the rest. Capped
         * at the FLORA block width so a header edit cannot walk past it. */
        for (v = 0; v < k_r5_flora_tree_count && v < TD5_TG_R5_FLORA_N; v++)
            tg_emit_real_page(&pages[tg_flora_tree_slot(v)],
                              k_r5_flora_tree_pal[v], k_r5_flora_tree_paln[v],
                              k_r5_flora_tree_idx[v], 1);
        /* Props: people/statue/animal (type 1), lamp glow (type 3 additive). */
        for (v = 0; v < TD5_TG_PROP_COUNT && v < k_real_prop_count; v++)
            tg_emit_real_page(&pages[tg_prop_slot(v)],
                              k_real_prop_pal[v], k_real_prop_paln[v],
                              k_real_prop_idx[v], k_prop_pages[v].type);
    } else {
        int v;
        tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_WALL], 0);
        for (v = 1; v < TD5_TG_WALL_VARIANTS; v++)
            tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_WALL_EXTRA + v - 1], v);
        /* [R7 item 4] procedural fill of the R7 variety pages -- distinct seeds
         * past the original variant range so each is its own wall. */
        for (v = 0; v < TD5_TG_R7_WALL_LOW_N; v++)
            tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_R7_WALL_LOW + v],
                                      TD5_TG_WALL_VARIANTS + v);
        for (v = 0; v < TD5_TG_R7_WALL_TOWER_N; v++)
            tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_R7_WALL_TOWER + v],
                                      TD5_TG_WALL_VARIANTS + TD5_TG_R7_WALL_LOW_N + v);
        for (v = 0; v < TD5_TG_STORE_VARIANTS; v++)
            tg_emit_texture_page_store(&pages[TD5_TG_PAGE_STORE + v], v);
        tg_emit_texture_page_green(&pages[TD5_TG_PAGE_GREEN]);
        /* Procedural trees vary by shape (deciduous/conifer). */
        for (v = 0; v < TD5_TG_TREE_VARIANTS; v++)
            tg_emit_texture_page_tree(&pages[tg_tree_slot(v)], k_tree_pages[v].shape);
        /* Procedural props by kind (person/statue/animal/lamp). */
        for (v = 0; v < TD5_TG_PROP_COUNT; v++)
            tg_emit_texture_page_prop(&pages[tg_prop_slot(v)], k_prop_pages[v].kind);
    }
    tg_emit_texture_page_rail(&pages[TD5_TG_PAGE_RAIL]);
    tg_emit_texture_page_water(&pages[TD5_TG_PAGE_WATER]);
    {   /* themed road-surface pages (variant 0 = base asphalt, already done) */
        int v;
        for (v = 1; v < TD5_TG_ROAD_VARIANTS; v++)
            tg_emit_texture_page_roadsurf(&pages[tg_road_slot(v)],
                                          k_road_surf[v].proc_kind);
    }
    /* [FB 2026-08-26] reserved feedback-batch pages -- one owner each. */
    tg_emit_texture_page_fb_city(&pages[TD5_TG_PAGE_SIDEWALK], 0);
    tg_emit_texture_page_fb_city(&pages[TD5_TG_PAGE_CROSSING], 1);
    if (tg_furniture_real_pages())
        tg_emit_real_page(&pages[TD5_TG_PAGE_FENCE], k_furn_fence_pal,
                          k_furn_fence_paln, k_furn_fence_idx, k_furn_fence_type);
    else
        tg_emit_texture_page_fb_city(&pages[TD5_TG_PAGE_FENCE], 2);
    tg_emit_texture_page_fb_treeline(&pages[TD5_TG_PAGE_TREELINE]);
    tg_emit_texture_page_fb_skyline(&pages[TD5_TG_PAGE_R4_SKYLINE]);  /* [R4 item 5] */
    tg_emit_texture_page_fb_tunnel(&pages[TD5_TG_PAGE_TUNNEL]);
    /* [R3 BRIDGE] deck surface (item 11) + 4 extra tunnel linings (item 16a). */
    tg_emit_texture_page_bridge_deck(&pages[TD5_TG_PAGE_BRIDGE_DECK]);
    {
        int v;
        for (v = 1; v < TD5_TG_TUNNEL_VARIANTS; v++)
            tg_emit_texture_page_tunnel_var(&pages[TD5_TG_PAGE_TUNNEL_VAR + v - 1], v);
    }
    /* [R6 TUNNEL item 8] proper concrete lining variants (b/c), portal header,
     * and emissive wall-lamp strip -- all in the reserved R6_TUNNEL page block. */
    {
        int v;
        for (v = 0; v < 4; v++)
            tg_emit_texture_page_r6_tunnel_lining(&pages[TD5_TG_PAGE_R6_TUNNEL + v], v);
    }
    tg_emit_texture_page_r6_tunnel_portal(&pages[TD5_TG_PAGE_R6_TUNNEL + 4]);
    tg_emit_texture_page_r6_tunnel_lamp(&pages[TD5_TG_PAGE_R6_TUNNEL + 5]);
    tg_emit_texture_page_fb_terrain(&pages[TD5_TG_PAGE_SNOW], 0);
    tg_emit_texture_page_fb_terrain(&pages[TD5_TG_PAGE_HILL], 1);
    tg_emit_texture_page_fb_banner(&pages[TD5_TG_PAGE_BANNER]);
    /* [FB r2] Real shipped furniture art. Unlike the level014 pages above these
     * are NOT behind TD5RE_AUTOTRACK_REAL_TEX: there is no procedural stand-in
     * worth keeping for a lamp post or a word, so they are the only artwork
     * these three page groups ever carry. */
    tg_emit_real_page(&pages[TD5_TG_PAGE_LAMPPOST], k_furn_lamp_pal,
                      k_furn_lamp_paln, k_furn_lamp_idx, k_furn_lamp_type);
    tg_emit_real_page(&pages[TD5_TG_PAGE_START_L], k_furn_start_l_pal,
                      k_furn_start_l_paln, k_furn_start_l_idx, k_furn_start_l_type);
    tg_emit_real_page(&pages[TD5_TG_PAGE_START_R], k_furn_start_r_pal,
                      k_furn_start_r_paln, k_furn_start_r_idx, k_furn_start_r_type);
    tg_emit_real_page(&pages[TD5_TG_PAGE_FINISH_L], k_furn_finish_l_pal,
                      k_furn_finish_l_paln, k_furn_finish_l_idx, k_furn_finish_l_type);
    tg_emit_real_page(&pages[TD5_TG_PAGE_FINISH_R], k_furn_finish_r_pal,
                      k_furn_finish_r_paln, k_furn_finish_r_idx, k_furn_finish_r_type);
    /* [R4 BRANCH item 8] concrete kerb face for avenue-median side walls. */
    tg_emit_texture_page_branch_kerb(&pages[TD5_TG_PAGE_BRANCH_KERB]);
    /* [R3 BLOCK] park & house art (feedback items 5-6). Slots +4..+9 of the
     * BLOCK reservation stay empty (unreferenced empty pages are free). */
    tg_emit_texture_page_r3_block(&pages[TD5_TG_PAGE_R3_BLOCK + 0], 0);  /* lawn  */
    tg_emit_texture_page_r3_block(&pages[TD5_TG_PAGE_R3_BLOCK + 1], 1);  /* hedge */
    tg_emit_texture_page_r3_block(&pages[TD5_TG_PAGE_R3_BLOCK + 2], 2);  /* wall  */
    tg_emit_texture_page_r3_block(&pages[TD5_TG_PAGE_R3_BLOCK + 3], 3);  /* roof  */
    /* [R4 BRIDGE] guardrail face (16b), pier/tower concrete (18), shore (20). */
    tg_emit_texture_page_r4_guardrail(&pages[TD5_TG_PAGE_R4_GUARDRAIL]);
    tg_emit_texture_page_r4_pier(&pages[TD5_TG_PAGE_R4_PIER]);
    tg_emit_texture_page_r4_coast(&pages[TD5_TG_PAGE_R4_COAST]);
    /* [R5 item 17a] armco steel W-beam guardrail (replaces the R4 concrete). */
    tg_emit_texture_page_r5_guardrail(&pages[TD5_TG_PAGE_R5_BRIDGE + 0]);
    /* [R6 item 13] alpha-keyed W-beam guardrail (transparent between beam/posts). */
    tg_emit_texture_page_r6_guardrail_alpha(&pages[TD5_TG_PAGE_R6_BRIDGE + 0]);
    /* [R6 item 17] per-style pier faces: steel plate + masonry ashlar. */
    tg_emit_texture_page_r6_pier_steel(&pages[TD5_TG_PAGE_R6_BRIDGE + 1]);
    tg_emit_texture_page_r6_pier_stone(&pages[TD5_TG_PAGE_R6_BRIDGE + 2]);
    /* [R7 item 9] concrete tunnel-portal facade (framed mouth, replaces the
     * R6 thin lintel). */
    tg_emit_texture_page_r7_tunnel_portal(&pages[TD5_TG_PAGE_R7_BRIDGE + 0]);
    /* [R4 CROSS item 9] cross-street asphalt with a longitudinal centre line. */
    tg_emit_texture_page_r4_cross(&pages[TD5_TG_PAGE_R4_CROSS + 0]);
    /* [R5 STRUCT item 1] solid concrete leg-face page for the gantry uprights. */
    tg_emit_texture_page_r5_leg(&pages[TD5_TG_PAGE_R5_LEG]);

    for (i = 0; i < count; i++) {
        if (pages[i].oom) {
            for (i = 0; i < count; i++) tg_buf_free(&pages[i]);
            return 0;
        }
    }

    tg_put_u32(out, count);
    for (i = 0; i < count; i++) {           /* absolute page offsets */
        tg_put_u32(out, cursor);
        cursor += (unsigned)pages[i].len;
    }
    for (i = 0; i < count; i++) {
        if (!tg_buf_need(out, pages[i].len)) break;
        memcpy(out->b + out->len, pages[i].b, pages[i].len);
        out->len += pages[i].len;
    }
    for (i = 0; i < count; i++) tg_buf_free(&pages[i]);

    TD5_LOG_I(LOG_TAG, "trackgen: textures = %u page(s), %zu bytes",
              count, out->len);
    return !out->oom;
}

/* ---------------------------------------------------------- config ------- */
void td5_trackgen_default_spec(TD5_TrackGenSpec *spec)
{
    if (!spec) return;
    memset(spec, 0, sizeof(*spec));
    spec->seed          = 0;
    spec->target_spans  = 1800;
    spec->lanes         = 4;
    spec->lane_width    = TD5_TG_LANE_WIDTH;
    spec->span_length   = TD5_TG_SPAN_LENGTH;
    spec->elevation_amplitude = 6000;
    spec->circuit       = 0;    /* point-to-point: no lap wrap, cheap ribbon */
    spec->curve_safety_x100 = 180;   /* 1.80 = the Python tool's 1.5 + headroom */
    spec->max_grade_x1000   = 120;   /* 0.120 = the Python tool's max_grade */
    spec->weight[TD5_TG_STRAIGHT]  = 35;
    spec->weight[TD5_TG_CURVE]     = 40;
    spec->weight[TD5_TG_ACUTE]     = 15;
    spec->weight[TD5_TG_DUAL_LANE] = 10;
}

void td5_trackgen_apply_config(TD5_TrackGenSpec *spec)
{
    if (!spec) return;
    spec->target_spans = td5_env_int("TD5RE_AUTOTRACK_SPANS",
                                     spec->target_spans, 60, TD5_TG_MAX_SPANS);
    /* 2..4 only: shipped tracks never exceed 4 lanes, so the rail LUTs
     * (td5_track.c:1315), edge masks and suspension paths are only exercised
     * in that range. Road WIDTH still varies freely. */
    spec->lanes        = td5_env_int("TD5RE_AUTOTRACK_LANES",
                                     spec->lanes, 2, 4);
    spec->elevation_amplitude =
        td5_env_int("TD5RE_AUTOTRACK_ELEVATION",
                    spec->elevation_amplitude, 0, 40000);
    spec->weight[TD5_TG_STRAIGHT] =
        td5_env_int("TD5RE_AUTOTRACK_PCT_STRAIGHT",
                    spec->weight[TD5_TG_STRAIGHT], 0, 100);
    spec->weight[TD5_TG_CURVE] =
        td5_env_int("TD5RE_AUTOTRACK_PCT_CURVE",
                    spec->weight[TD5_TG_CURVE], 0, 100);
    spec->weight[TD5_TG_ACUTE] =
        td5_env_int("TD5RE_AUTOTRACK_PCT_ACUTE",
                    spec->weight[TD5_TG_ACUTE], 0, 100);
    spec->weight[TD5_TG_DUAL_LANE] =
        td5_env_int("TD5RE_AUTOTRACK_PCT_DUAL",
                    spec->weight[TD5_TG_DUAL_LANE], 0, 100);
    spec->curve_safety_x100 =
        td5_env_int("TD5RE_AUTOTRACK_CURVESAFE",
                    spec->curve_safety_x100, 100, 800);
    spec->max_grade_x1000 =
        td5_env_int("TD5RE_AUTOTRACK_GRADE",
                    spec->max_grade_x1000, 0, 200);
}

/* ----------------------------------------------------------- build ------- */
int td5_trackgen_build_level(const TD5_TrackGenSpec *spec, int level_num,
                             int *out_spans)
{
    char dir[256];
    TG_NodeList nl;
    TG_Buf strip, left, right, info;
    int tally[TD5_TG_SECTION_COUNT];
    int nspans = 0, ok = 0;

    if (!spec) return 0;

    memset(&nl, 0, sizeof(nl));
    memset(&strip, 0, sizeof(strip));
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    memset(&info, 0, sizeof(info));
    memset(tally, 0, sizeof(tally));

    /* [R2 item 24] Clear the inventory BEFORE anything is emitted. Placed here
     * rather than in regenerate so a direct build_level call (the S2 regen
     * self-check, a future editor) reports its own elements and not the
     * previous build's. */
    tg_acct_reset();

    /* [R2 item 23] Lay the biome grid out BEFORE the centerline walk: the strip
     * emitter asks tg_surface_attr for every span, so the grid has to exist by
     * then. Driven by the seed, not by the geometry RNG, so it never perturbs
     * the road. Laid out for the whole cell array rather than for nspans, so a
     * span past the ring (an appended branch corridor) still has a biome. */
    tg_biome_layout(spec->seed);

    tg_srand(spec->seed);

    snprintf(dir, sizeof(dir), "re/assets/levels/level%03d", level_num);
    _mkdir(dir);

    if (!tg_build_centerline(spec, &nl, tally)) {
        TD5_LOG_E(LOG_TAG, "trackgen: centerline build failed");
        goto done;
    }
    tg_apply_elevation(spec, &nl);

    if (td5_env_flag_off("TD5RE_AUTOTRACK_SELFCHECK")) {
        tg_selfcheck_ranges(&nl, spec->lanes,
                            td5_env_int("TD5RE_AUTOTRACK_BLOCK",
                                        TD5_TG_ORIGIN_BLOCK, 1, 20));
        s_selfcheck_regen_seed = spec->seed;   /* run after the build completes */
    }

    if (!tg_emit_strip(&nl, &strip, &nspans) || nspans < 8) {
        TD5_LOG_E(LOG_TAG, "trackgen: strip emit failed (spans=%d)", nspans);
        goto done;
    }
    /* [R3 item 17] Surface any floor/road-overlap or suspect fork geometry now
     * that the strip (and s_ring_len) exist -- read-only, one-off. */
    tg_validate_geometry_safety(&nl, nspans);
    /* Routes must cover exactly the ring the strip header declares. */
    /* byte0 is the lateral corridor position (0 = left rail, 255 = right).
     * Straddle the centreline symmetrically so the AI's racing line runs down
     * the middle of a road whose width varies from section to section. */
    if (!tg_emit_routes(&nl, nspans, 96,  &left) ||
        !tg_emit_routes(&nl, nspans, 160, &right)) {
        TD5_LOG_E(LOG_TAG, "trackgen: route emit failed");
        goto done;
    }
    if (!tg_emit_levelinf(spec, nspans, &info) || info.len != 100) {
        TD5_LOG_E(LOG_TAG, "trackgen: levelinf emit failed (len=%zu)", info.len);
        goto done;
    }

    if (!tg_write_file(dir, "STRIP.DAT", strip.b, strip.len) ||
        !tg_write_file(dir, "LEFT.TRK",  left.b,  left.len)  ||
        !tg_write_file(dir, "RIGHT.TRK", right.b, right.len) ||
        !tg_write_file(dir, "LEVELINF.DAT", info.b, info.len)) {
        goto done;
    }

    /* MODELS.DAT is OPT-IN (TD5RE_AUTOTRACK_SCENERY=1) and all-or-nothing:
     * its mere presence disables the procedural ribbon renderer, so if the
     * mesh bytes are wrong the road goes INVISIBLE (still drivable). Default
     * off keeps the verified ribbon path as shipped. A stale MODELS.DAT from a
     * previous opt-in run would silently keep the ribbon disabled, so remove
     * it when the knob is off. */
    {
        char models_path[320];
        snprintf(models_path, sizeof(models_path), "%s/MODELS.DAT", dir);
        /* Scenery is now DEFAULT ON (textured road, buildings, bridges,
         * biomes, tree billboards -- all verified in frame). Set
         * TD5RE_AUTOTRACK_SCENERY=0 to fall back to the untextured procedural
         * ribbon. */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_SCENERY")) {
            TG_Buf models, tex;
            memset(&models, 0, sizeof(models));
            memset(&tex, 0, sizeof(tex));
            if (tg_emit_models(&nl, nspans, spec->lanes, &models))
                tg_write_file(dir, "MODELS.DAT", models.b, models.len);
            else
                TD5_LOG_W(LOG_TAG, "trackgen: models emit failed; "
                          "falling back to the ribbon renderer");
            /* Texture pages are only referenced by the mesh, so they follow
             * the same gate -- without MODELS.DAT nothing samples them. */
            if (tg_emit_textures(&tex))
                tg_write_file(dir, "TEXTURES.DAT", tex.b, tex.len);
            tg_buf_free(&models);
            tg_buf_free(&tex);
        } else {
            char tex_path[320];
            snprintf(tex_path, sizeof(tex_path), "%s/TEXTURES.DAT", dir);
            remove(models_path);
            remove(tex_path);
        }
    }

    tg_install_sky(dir, spec->seed);

    TD5_LOG_I(LOG_TAG, "trackgen: seed=%u level=%d spans=%d len=%.0f world units",
              spec->seed, level_num, nspans,
              (double)nspans * (double)spec->span_length);
    {
        int s;
        for (s = 0; s < TD5_TG_SECTION_COUNT; s++)
            TD5_LOG_I(LOG_TAG, "trackgen:   %-10s x%d (weight %d)",
                      tg_section_name((TD5_TrackGenSection)s), tally[s],
                      spec->weight[s]);
    }
    {   /* Biome layout, so a run can be checked against what is on screen.
         * [R2 item 23] Logged as MERGED runs (consecutive same-biome cells
         * collapsed) with each biome's feature weighting, so "why are there no
         * tunnels here" and "why did the scenery change" are both answerable
         * from the log. */
        int s = 0, runs = 0;
        while (s < nspans) {
            int a, z, b = tg_biome_cell_index(s);
            tg_biome_run_bounds(s, &a, &z);
            if (z >= nspans) z = nspans - 1;
            TD5_LOG_I(LOG_TAG,
                      "trackgen:   biome run %2d: spans %5d-%5d %-11s "
                      "(climate=%d urbanity=%d bridge=%d%% tunnel=%d%% surf=%s)",
                      runs, a, z, k_biomes[b].name,
                      k_biomes[b].climate, k_biomes[b].urbanity,
                      tg_biome_bridge_pct(a), tg_biome_tunnel_pct(a),
                      k_road_surf[k_biomes[b].road_surf].grip_class == 1
                          ? "tarmac" : "special");
            runs++;
            s = z + 1;
        }
        TD5_LOG_I(LOG_TAG,
                  "trackgen: %d biome run(s), cell=%d spans, blend=%d spans, "
                  "time=%s",
                  runs, TD5_TG_BIOME_RUN,
                  td5_env_int("TD5RE_AUTOTRACK_BIOME_BLEND",
                              TD5_TG_BIOME_BLEND, 0, TD5_TG_BIOME_RUN / 2),
                  s_is_night ? "NIGHT" : "DAY");
    }
    {   /* Branch nodes are held in s_forks rather than emitted through a single
         * call site, so they are accounted here where the table is complete. */
        int f;
        for (f = 0; f < s_fork_count; f++) {
            tg_acct_range(TG_ACCT_BRANCH, s_forks[f].F, s_forks[f].R);
            tg_acct_range(TG_ACCT_BRANCH, s_forks[f].cbase,
                          s_forks[f].cbase + s_forks[f].len - 1);
        }
    }
    tg_acct_report(nspans);
    ok = 1;

done:
    if (out_spans) *out_spans = nspans;
    free(nl.v);
    tg_buf_free(&strip);
    tg_buf_free(&left);
    tg_buf_free(&right);
    tg_buf_free(&info);
    return ok;
}

/* ===================== [S2] REGENERATE FOR IN-PLACE REWRITE =====================
 * Rebuild the main-road span records for `seed` and hand them back as a blob of
 * 24-byte records. Phase 2 streaming needs this so the track module can
 * overwrite a REGION of its live span array with bytes that provably match what
 * the same seed produced originally
 * (docs/plans/AUTOTRACK_STREAMING.md, stage S2).
 *
 * Ownership is deliberate: the generator produces bytes, the track module owns
 * its arrays and does the writing. This function never touches live state.
 *
 * Deterministic by construction -- tg_srand(seed) resets the private xorshift,
 * and nothing here draws from the game's rand(), so a later call with the same
 * seed reproduces the same bytes regardless of what happened in between.
 *
 * Caller frees *out_bytes. Returns 1 on success.
 */
int td5_trackgen_regenerate_main_spans(unsigned int seed,
                                      unsigned char **out_bytes,
                                      int *out_span_count)
{
    TD5_TrackGenSpec spec;
    TG_NodeList nl;
    TG_Buf spans, verts;
    int vtx_count = 0, ok = 0, block;

    if (!out_bytes || !out_span_count) return 0;
    *out_bytes = NULL;
    *out_span_count = 0;

    td5_trackgen_default_spec(&spec);
    td5_trackgen_apply_config(&spec);
    spec.seed = seed;

    memset(&nl, 0, sizeof(nl));
    memset(&spans, 0, sizeof(spans));
    memset(&verts, 0, sizeof(verts));

    tg_srand(spec.seed);
    block = td5_env_int("TD5RE_AUTOTRACK_BLOCK", TD5_TG_ORIGIN_BLOCK, 1, 20);

    {
        int tally[TD5_TG_SECTION_COUNT];
        memset(tally, 0, sizeof(tally));
        if (tg_build_centerline(&spec, &nl, tally)) {
            tg_apply_elevation(&spec, &nl);
            if (tg_emit_span_range(&nl, 0, nl.count - 1, spec.lanes, block,
                                   &spans, &verts, &vtx_count)) {
                *out_bytes = spans.b;          /* hand the buffer over */
                *out_span_count = (int)(spans.len / 24);
                spans.b = NULL;                /* so tg_buf_free does not free it */
                spans.len = spans.cap = 0;
                ok = 1;
            }
        }
    }

    free(nl.v);
    tg_buf_free(&spans);
    tg_buf_free(&verts);

    if (ok)
        TD5_LOG_I(LOG_TAG, "trackgen: regenerated %d main span records for "
                  "seed %u (%d bytes)", *out_span_count, seed,
                  *out_span_count * 24);
    else
        TD5_LOG_E(LOG_TAG, "trackgen: regenerate for seed %u FAILED", seed);
    return ok;
}

/* [S2 GATE, generator half] Regenerating twice from one seed must give
 * identical bytes -- otherwise an in-place rewrite could not be trusted to
 * reproduce what the track was built from. Cheap, so it runs with the same
 * TD5RE_AUTOTRACK_SELFCHECK knob as the S1 gate. */
static void tg_selfcheck_regen(unsigned int seed)
{
    unsigned char *a = NULL, *b = NULL;
    int na = 0, nb = 0;

    if (!td5_trackgen_regenerate_main_spans(seed, &a, &na) ||
        !td5_trackgen_regenerate_main_spans(seed, &b, &nb)) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: regen FAILED");
    } else if (na != nb || memcmp(a, b, (size_t)na * 24) != 0) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: regen NOT deterministic "
                  "(%d vs %d spans)", na, nb);
    } else {
        TD5_LOG_I(LOG_TAG, "trackgen selfcheck: regen PASS -- seed %u "
                  "reproduces %d identical span records on a second call",
                  seed, na);
    }
    free(a);
    free(b);
}

/* ------------------------------------------------------- lifecycle ------- */
int td5_trackgen_init(void)
{
    /* Build one track at boot so the AUTO-GENERATED entry is present (and
     * loadable) from the main menu onwards. Each race launch regenerates with
     * a fresh seed -- see td5_asset_load_level. A failure here is non-fatal:
     * the entry simply never registers and the selector skips it. */
    if (!td5_trackgen_regenerate(0))
        TD5_LOG_W(LOG_TAG, "trackgen: boot generation failed; "
                  "AUTO-GENERATED will not be selectable");
    return 1;
}

void td5_trackgen_shutdown(void)
{
    s_last_seed = 0;
}

/* -------------------------------------------------------- identity ------- */
int td5_trackgen_level_number(void) { return TD5_TG_LEVEL_NUM; }
int td5_trackgen_slot(void)         { return TD5_TG_SLOT; }

int td5_trackgen_is_auto_slot(int slot) { return slot == TD5_TG_SLOT; }

unsigned int td5_trackgen_last_seed(void) { return s_last_seed; }

int td5_trackgen_regenerate(unsigned int seed)
{
    TD5_TrackGenSpec spec;
    int spans = 0;

    td5_trackgen_default_spec(&spec);
    td5_trackgen_apply_config(&spec);

    /* TD5RE_AUTOTRACK_SEED pins the seed so two runs generate the IDENTICAL
     * track. Without this any A/B measurement compares two different random
     * roads and attributes the difference to whatever knob was changed. */
    if (seed == 0) {
        int pinned = td5_env_int("TD5RE_AUTOTRACK_SEED", 0, 0, 0x7FFFFFFF);
        seed = pinned ? (unsigned int)pinned
                      : (unsigned int)td5_plat_time_ms() * 2654435761u
                        + 0x9E3779B9u;
    }
    spec.seed = seed;

    /* [R2 item 22] Time of day is decided HERE -- regenerate is what a race
     * launch calls, so this is "on entering the race" -- and BEFORE the build,
     * so every emitter that asks td5_trackgen_is_night() during it agrees. */
    tg_decide_night(seed);

    if (!td5_trackgen_build_level(&spec, TD5_TG_LEVEL_NUM, &spans)) {
        TD5_LOG_E(LOG_TAG, "trackgen: regenerate failed; auto track unavailable");
        return 0;
    }

    s_last_seed = seed;

    if (s_selfcheck_regen_seed) {
        unsigned int sd = s_selfcheck_regen_seed;
        s_selfcheck_regen_seed = 0;
        tg_selfcheck_regen(sd);
    }

    /* [R3 item 18] Finish span for the registry MUST be the same MAIN-RING span
     * that tg_emit_levelinf placed the last checkpoint on -- tg_finish_span(ring)
     * -- NOT `spans - 4`.
     *
     * `spans` is the FULL emitted strip count, which also counts every fork's pad
     * and appended corridor tail (with branches on seed 99991 the main ring is
     * ~1800 spans but `spans` is ~1987). The registry finish span is consumed by
     * advance_pending_finish_state as s_td6_finish_span, and that P2P branch takes
     * PRECEDENCE over the LEVELINF checkpoints (it returns before reaching the
     * checkpoint-crossing code). So `spans - 4` put the finish line on a CORRIDOR
     * tail (~1983) that a normal main-line drive never reaches: the race could
     * only finish by driving ~180 spans into a second lap, which is why the user
     * saw the race never finish and race.log logged zero checkpoint-pass events
     * (that code was dead). Deriving the finish from s_ring_len makes the registry
     * finish, the LEVELINF last checkpoint and the finish banner all agree on one
     * on-ring span. */
    {
        int ring   = (s_ring_len > 0) ? s_ring_len : spans;
        int finish = tg_finish_span(ring);
        if (finish <= 0)   /* ring too short for a placed finish: last-resort */
            finish = (spans > 8) ? spans - 4 : spans - 1;
        td5_track_registry_set_auto(TD5_TG_SLOT, TD5_TG_LEVEL_NUM,
                                   "AUTO-GENERATED", spec.circuit,
                                   TD5_TG_GRID_SPAN, finish);
        TD5_LOG_I(LOG_TAG, "trackgen: registry finish span=%d (main ring=%d, full "
                  "strip=%d; old spans-4 would be %d)", finish, ring, spans,
                  spans > 8 ? spans - 4 : spans - 1);
    }

    TD5_LOG_I(LOG_TAG, "trackgen: auto track ready (slot %d, level %d, "
              "seed %u, %d spans)", TD5_TG_SLOT, TD5_TG_LEVEL_NUM, seed, spans);
    return 1;
}
