/**
 * td5_roam_barrier_stub.c -- TEMPORARY stand-in for F6-A's td5_roam_barrier.c (PORT-ONLY, F6-B).
 * The F6-B branch builds against the frozen contract (td5_roam_barrier.h) before F6-A's
 * implementation is cherry-picked; this file is deleted (and dropped from srcs.txt) then.
 * An empty barrier set: the race runs, nothing is closed.
 */
#include <string.h>
#include "td5_roam_barrier.h"

static int s_visible;

int  td5_roam_barriers_build(const TD5_RoamRouteResult *route) { (void)route; return 0; }
const TD5_RoamBarrier *td5_roam_barriers(int *count) { if (count) *count = 0; return NULL; }
void td5_roam_barriers_clear(void) { }
void td5_roam_barriers_set_visible(int on) { s_visible = on; }
int  td5_roam_barriers_visible(void) { return s_visible; }
void td5_roam_barriers_draw(int view) { (void)view; }
void td5_roam_barriers_stats(TD5_RoamBarrierStats *out) { if (out) memset(out, 0, sizeof *out); }
