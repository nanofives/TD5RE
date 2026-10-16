/**
 * td5_roam_mode.c -- GEO FREE ROAM: the ROAM game mode (PORT-ONLY, F2 task A).
 * Contract: td5_roam_mode.h. STUB at the contract commit: state only, no gating yet.
 */
#include <string.h>
#include "td5_roam_mode.h"
#include "td5_roam_locate.h"

#define ROAM_MAX_SLOTS 32   /* == TD5_MAX_TOTAL_ACTORS */

typedef struct {
    int                             active;
    TD5_RoamSpawn                   spawn;
    int32_t                         origin_x, origin_z;
    const struct TD5_RoamResident  *resident;
    uint64_t                        slot_edge[ROAM_MAX_SLOTS];
} RoamSession;

static RoamSession s_roam;

int td5_roam_active(void) { return s_roam.active; }

void td5_roam_begin(const TD5_RoamSpawn *s)
{
    if (!s) return;
    s_roam.spawn = *s;
    s_roam.active = 1;
}

void td5_roam_end(void)
{
    s_roam.active = 0;
}

void td5_roam_world_origin(int32_t *ox, int32_t *oz)
{
    if (ox) *ox = s_roam.origin_x;
    if (oz) *oz = s_roam.origin_z;
}

const struct TD5_RoamResident *td5_roam_resident(void) { return s_roam.resident; }
void td5_roam_set_resident(const struct TD5_RoamResident *rs) { s_roam.resident = rs; }
const TD5_RoamSpawn *td5_roam_spawn(void) { return s_roam.active ? &s_roam.spawn : NULL; }

uint64_t td5_roam_slot_edge(int slot)
{
    return (slot >= 0 && slot < ROAM_MAX_SLOTS) ? s_roam.slot_edge[slot] : 0;
}

void td5_roam_set_slot_edge(int slot, uint64_t edge_key)
{
    if (slot >= 0 && slot < ROAM_MAX_SLOTS) s_roam.slot_edge[slot] = edge_key;
}
