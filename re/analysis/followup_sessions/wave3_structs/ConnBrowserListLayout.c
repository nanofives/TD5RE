/* ConnBrowserListLayout — recovered from:
 *   CreateFrontendDisplayModeButton       @ 0x00425de0  (writer)
 *   UpdateFrontendDisplayModeSelection    @ 0x00426580  (navigation, sentinel walk)
 *   RenderFrontendDisplayModeHighlight    @ 0x004263e0  (highlight blit)
 *   MoveFrontendSpriteRect                @ ~0x004258xx (mouse hit-test)
 *   RenderFrontendUiRects                 @ ~0x004259xx (frame draw)
 *
 * Base: g_connBrowserListOriginX_PROVISIONAL @ 0x00499c78
 * Stride: 0x34 bytes (52) per entry — confirmed by `iVar8 * 0xd` (int-stride 13) and
 *         `iVar8 * 0x34` (byte-stride for byte field access) in CreateFrontendDisplayModeButton.
 * Array length: 64 entries (sentinel walk in UpdateFrontendDisplayModeSelection stops when
 *         `(&DAT_00499cbc)[idx*0xd] == -1` — sentinel field is at +0x44/(idx+1)*+0x10 of next).
 *         End boundary observed: address comparison `< 0x49a988` ⇒ (0x49a988-0x499c88)/0x34 = 64.
 * Total table size: 64 * 0x34 = 0xD00 bytes.
 * Source: Tier1-B 2026-05-22
 *
 * NOTE: W1-E sized this cluster as 76 B because only the first entry's fields had
 * named/hot references. The single-entry struct is 52 B; the table is 64 of them.
 *
 * Field +0x2c (flags_or_render_state) bit semantics (from UpdateFrontendDisplayModeSelection):
 *   bit 0 (0x01): default — active (set on create)
 *   bit 1 (0x02): two-axis navigation enabled (LEFT/RIGHT also valid, not just UP/DOWN)
 *   bit 2 (0x04): disabled — skip during navigation hit-test (mask `& 4` repeatedly checked)
 *   value 5 (0x05): set when DAT_0049b694 != 0 — overlay mode active
 *
 * Field +0x10 (sentinel_or_count_1) and +0x14 (sentinel_or_count_2) are written 0 on create.
 * The "next entry's +0x10" being -1 is the iteration sentinel.
 *
 * Field +0x30 (select_progress) is a 0..6 counter incremented per-frame while button is
 * hovered (see UpdateFrontendDisplayModeSelection first loop). W1-E provisionally named
 * this "row_stride" — the audit shows it's actually a hover-press progress counter.
 */
typedef struct ConnBrowserListLayout {
    /* +0x00 */ int    origin_x;            /* button top-left X in frontend canvas coords */
    /* +0x04 */ int    origin_y;            /* button top-left Y */
    /* +0x08 */ int    end_x;               /* origin_x + width */
    /* +0x0c */ int    end_y;               /* origin_y + height */
    /* +0x10 */ int    sentinel_or_count_1; /* set 0 on create; -1 = end-of-table sentinel */
    /* +0x14 */ int    sentinel_or_count_2; /* set 0 on create */
    /* +0x18 */ int    width;               /* surface width in pixels */
    /* +0x1c */ int    height;              /* surface height in pixels */
    /* +0x20 */ int    user_data;           /* opaque caller token (param_6) — usually screen id */
    /* +0x24 */ void * surface_ptr;         /* DirectDrawSurface* (CreateTrackedFrontendSurface result) */
    /* +0x28 */ int    surface_registry_id; /* GetFrontendSurfaceRegistryId(surface_ptr) */
    /* +0x2c */ int    flags;               /* bit0=active, bit1=two-axis-nav, bit2=disabled */
    /* +0x30 */ int    select_progress;     /* 0..6 hover-press counter (W1-E mislabel: not row_stride) */
} ConnBrowserListLayout; /* size 0x34 */

/* Array form (production storage):
 *   ConnBrowserListLayout g_connBrowserListTable[64];   // base 0x00499c78
 *
 * Single-entry C struct is the right unit to type into Ghidra; readers/writers can
 * then be re-decomp'd as `g_connBrowserListTable[idx].field` instead of
 * `(&DAT_xxxx)[idx * 0xd]`.
 */

/* type_define_c-ready definition above. Apply via ghidra-apply Wave 2E phase. */
