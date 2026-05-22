/* PolygonClipperDrawState — recovered from:
 *   ClipAndSubmitProjectedPolygon         @ 0x004317f0
 *   SetProjectedClipRect                  @ 0x0043e640
 *   RenderTrackSegmentBatch (called from clip pipeline)
 *   RenderTrackSegmentBatchVariant
 *   AppendClippedPolygonTriangleFan
 *
 * Base: g_currentDrawCallVertexBuffer @ 0x004afb14
 * Size: 0x40 bytes (64) — base..0x004afb50+4
 * Source: Tier1-B 2026-05-22
 *
 * SetProjectedClipRect writes clip_left/right/top/bottom (params 1,2,3,4)
 * after clamping against the raw_clip_x_min/max + raw_clip_y_min/max bounds,
 * then caches half-width/height as (right-left)*0.5 and (bottom-top)*0.5.
 *
 * ClipAndSubmitProjectedPolygon reads clip_left/right/top/bottom at every
 * 4-edge clip stage and uses half_width/half_height for early-out screen-space
 * rejection (`uVar11 = (half_height - abs(scratch_y)) >> 31 …`).
 *
 * The tmp_y_scratch field (+0x04) is a per-vertex scratch used during the
 * projection inner loop (`pfVar18[1] * pfVar18[2] * g_projectionDepth`)
 * and read once via abs() for the early-out test. Not externally visible.
 */
typedef struct PolygonClipperDrawState {
    /* +0x00 */ float * vertex_buffer;        /* g_currentDrawCallVertexBuffer — D3D vertex scratch ptr */
    /* +0x04 */ float   tmp_y_scratch;        /* per-vertex Y scratch during projection */
    /* +0x08 */ unsigned int _pad_08;         /* padding, no refs */
    /* +0x0c */ float   clip_left;            /* active clip rect — left edge   (SetProjectedClipRect param_1) */
    /* +0x10 */ float   clip_right;           /* active clip rect — right edge  (param_2) */
    /* +0x14 */ float   clip_top;             /* active clip rect — top edge    (param_3) */
    /* +0x18 */ float   clip_bottom;          /* active clip rect — bottom edge (param_4) */
    /* +0x1c */ float   clip_half_width;      /* (clip_right - clip_left) * 0.5f */
    /* +0x20 */ float   clip_half_height;     /* (clip_bottom - clip_top) * 0.5f */
    /* +0x24 */ float   raw_clip_x_min;       /* hard floor used to clamp incoming param_1/_2 */
    /* +0x28 */ float   raw_clip_x_max;
    /* +0x2c */ float   raw_clip_y_min;       /* hard floor used to clamp incoming param_3/_4 */
    /* +0x30 */ float   raw_clip_y_max;
    /* +0x34 */ int *   index_buffer;         /* g_currentDrawCallIndexBuffer — D3D index scratch ptr */
    /* +0x38 */ int     vertex_count;         /* g_currentDrawCallVertexCount */
    /* +0x3c */ int     index_count;          /* g_currentDrawCallIndexCount */
} PolygonClipperDrawState; /* size 0x40 */

/* type_define_c-ready definition above. Apply via ghidra-apply Wave 2E phase. */
