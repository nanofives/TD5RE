/* td5_rcmd.h — per-pane CPU render command list (Phase B render-transform).
 *
 * The route to a split-screen FPS gain WITHOUT D3D11 deferred contexts (which
 * don't replay on this driver). Each pane's expensive CPU work (track walk,
 * mesh transform, frustum cull) runs on a worker thread and RECORDS the
 * resulting platform-level render calls into a per-pane command list — copying
 * geometry into private arenas and touching NO shared GPU state (the texture
 * cache lookup is deferred). The main thread then REPLAYS each pane's list in
 * order onto the immediate context (live GPU). Workers record concurrently
 * (per-pane lists, __thread-selected → verified isolated); replay is serial.
 *
 * Recording is selected per-thread: while a list is active on this thread, the
 * record-aware platform render fns append a command instead of issuing GPU work.
 */
#ifndef TD5_RCMD_H
#define TD5_RCMD_H

#include "td5_platform.h"   /* TD5_D3DVertex, TD5_RenderPreset */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RCmdList RCmdList;

RCmdList *td5_rcmd_create(void);          /* heap-allocate an empty list        */
void      td5_rcmd_reset(RCmdList *l);    /* clear contents, keep capacity      */

void      td5_rcmd_begin(RCmdList *l);    /* this thread starts recording into l */
void      td5_rcmd_end(void);             /* this thread stops recording         */
int       td5_rcmd_recording(void);       /* 1 if this thread is recording       */

/* Record ops — called by the record-aware platform/render layer. No-op if this
 * thread isn't recording (callers gate on td5_rcmd_recording()). */
void td5_rcmd_set_preset(int preset);
void td5_rcmd_set_fog(int enable, uint32_t color, float start, float end, float density);
void td5_rcmd_bind_texture(int slot);   /* direct page->GPU bind (no cache) */
void td5_rcmd_bind_page(int page);      /* cache-managed bind, resolved at replay */
void td5_rcmd_set_viewport(int x, int y, int w, int h);
void td5_rcmd_set_clip(int l, int t, int r, int b);
void td5_rcmd_draw_tris(const TD5_D3DVertex *v, int vc, const uint16_t *idx, int ic);
void td5_rcmd_draw_lines(const TD5_D3DVertex *v, int vc);

/* Replay a recorded list on the MAIN thread (issues live GPU calls). Must be
 * called with no list active on this thread (so the replayed calls run live). */
void td5_rcmd_replay(RCmdList *l);

#ifdef __cplusplus
}
#endif

#endif /* TD5_RCMD_H */
