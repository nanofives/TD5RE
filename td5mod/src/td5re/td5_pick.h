/**
 * td5_pick.h -- dev-only free-cam geometry/texture PICKER (PORT-ONLY, DEV-ONLY).
 *
 * While the free camera is active (pause menu -> FREE CAMERA), hovering the
 * mouse over track/scenery geometry highlights the mesh under the cursor and
 * shows its identity; a left-click copies that identity as JSON to the Windows
 * clipboard, so a change can be requested precisely ("entry 214 slot 3, page
 * 41, pos ...") instead of in prose.
 *
 * Pure render-side + input-side: it NEVER touches the sim (physics/AI/replay),
 * so determinism and golden traces are unaffected by construction -- the same
 * contract the free camera itself follows. Compiled out of RELEASE.
 *
 * Knob: TD5RE_PICK=0 disables it (default on in dev builds).
 */
#ifndef TD5_PICK_H
#define TD5_PICK_H

#include "td5_types.h"

/* Once per rendered frame, BEFORE the world pass: capture the cursor and arm
 * collection iff the free camera is active. */
void td5_pick_begin_frame(void);

/* True while begin_frame armed collection this frame. Gates the per-mesh
 * consider() call in the render walk so it costs nothing when off. */
int  td5_pick_collecting(void);

/* Offer one visible track mesh as a hover candidate. cx/cy/cz/r are the mesh
 * bounding sphere in RENDER-FLOAT world space (world/256), exactly as the
 * render walk holds them. `block` + `slot` identify the mesh. */
void td5_pick_consider(const TD5_SpanDisplayList *block, int slot,
                       const TD5_MeshHeader *mesh,
                       float cx, float cy, float cz, float r);

/* After the world pass (serial path, viewport 0): resolve the hovered mesh,
 * draw the highlight box, and on a left-click edge copy the identity JSON to
 * the clipboard. Emits its own debug-line batch (reset/flush inside). */
void td5_pick_finish_frame(void);

/* Queue the picker's HUD label (called from the HUD text pass). No-op unless a
 * mesh is currently hovered. */
void td5_pick_hud_draw(void);

#endif /* TD5_PICK_H */
