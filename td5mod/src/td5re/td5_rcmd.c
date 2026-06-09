/* td5_rcmd.c — per-pane CPU render command list. See td5_rcmd.h. */
#include "td5_rcmd.h"
#include "td5_render.h"     /* td5_render_bind_texture_page */
#include "td5_platform.h"
#include <stdlib.h>
#include <string.h>

typedef enum {
    RC_PRESET, RC_FOG, RC_BIND_TEX, RC_BIND_PAGE,
    RC_VIEWPORT, RC_CLIP, RC_DRAW_TRIS, RC_DRAW_LINES
} RCType;

typedef struct {
    RCType   type;
    int      a, b, c, d;     /* generic int params (preset/page/slot/rect/fog-enable) */
    uint32_t u;              /* fog color */
    float    f0, f1, f2;     /* fog start/end/density */
    uint32_t voff;           /* byte offset into vtx arena (draws) */
    uint32_t ioff;           /* byte offset into idx arena (draws) */
    int      vc, ic;         /* vertex / index counts (draws) */
} RCmd;

struct RCmdList {
    RCmd    *cmds;  int    ccount, ccap;
    uint8_t *vtx;   size_t vsize, vcap;
    uint8_t *idx;   size_t isize, icap;
};

/* Active recording list for THIS thread. __thread is verified per-worker isolated
 * on the job pool (td5_jobs_selftest_tls == 0). NULL => not recording (live). */
static __thread RCmdList *t_active = NULL;

int  td5_rcmd_recording(void) { return t_active != NULL; }
void td5_rcmd_begin(RCmdList *l) { t_active = l; }
void td5_rcmd_end(void) { t_active = NULL; }

RCmdList *td5_rcmd_create(void) { return (RCmdList *)calloc(1, sizeof(RCmdList)); }
void td5_rcmd_reset(RCmdList *l) { if (l) { l->ccount = 0; l->vsize = 0; l->isize = 0; } }

static RCmd *push_cmd(RCmdList *l)
{
    if (l->ccount >= l->ccap) {
        int nc = l->ccap ? l->ccap * 2 : 2048;
        RCmd *p = (RCmd *)realloc(l->cmds, (size_t)nc * sizeof(RCmd));
        if (!p) return NULL;
        l->cmds = p; l->ccap = nc;
    }
    return &l->cmds[l->ccount++];
}

static uint32_t push_bytes(uint8_t **buf, size_t *size, size_t *cap,
                           const void *data, size_t bytes)
{
    if (*size + bytes > *cap) {
        size_t nc = *cap ? *cap : (1u << 20);   /* start 1 MB */
        while (nc < *size + bytes) nc *= 2;
        uint8_t *p = (uint8_t *)realloc(*buf, nc);
        if (!p) return 0xFFFFFFFFu;
        *buf = p; *cap = nc;
    }
    uint32_t off = (uint32_t)*size;
    memcpy(*buf + off, data, bytes);
    *size += bytes;
    return off;
}

void td5_rcmd_set_preset(int preset)
{ RCmd *c = push_cmd(t_active); if (c) { c->type = RC_PRESET; c->a = preset; } }

void td5_rcmd_set_fog(int enable, uint32_t color, float start, float end, float density)
{ RCmd *c = push_cmd(t_active); if (c) { c->type = RC_FOG; c->a = enable; c->u = color; c->f0 = start; c->f1 = end; c->f2 = density; } }

void td5_rcmd_bind_texture(int slot)
{ RCmd *c = push_cmd(t_active); if (c) { c->type = RC_BIND_TEX; c->a = slot; } }

void td5_rcmd_bind_page(int page)
{ RCmd *c = push_cmd(t_active); if (c) { c->type = RC_BIND_PAGE; c->a = page; } }

void td5_rcmd_set_viewport(int x, int y, int w, int h)
{ RCmd *c = push_cmd(t_active); if (c) { c->type = RC_VIEWPORT; c->a = x; c->b = y; c->c = w; c->d = h; } }

void td5_rcmd_set_clip(int l, int t, int r, int b)
{ RCmd *c = push_cmd(t_active); if (c) { c->type = RC_CLIP; c->a = l; c->b = t; c->c = r; c->d = b; } }

void td5_rcmd_draw_tris(const TD5_D3DVertex *v, int vc, const uint16_t *idx, int ic)
{
    RCmdList *l = t_active;
    RCmd *c;
    if (!l || vc <= 0) return;
    c = push_cmd(l);
    if (!c) return;
    c->type = RC_DRAW_TRIS;
    c->voff = push_bytes(&l->vtx, &l->vsize, &l->vcap, v, (size_t)vc * sizeof(TD5_D3DVertex));
    c->vc   = vc;
    if (idx && ic > 0) {
        c->ioff = push_bytes(&l->idx, &l->isize, &l->icap, idx, (size_t)ic * sizeof(uint16_t));
        c->ic   = ic;
    } else {
        c->ioff = 0xFFFFFFFFu; c->ic = 0;
    }
}

void td5_rcmd_draw_lines(const TD5_D3DVertex *v, int vc)
{
    RCmdList *l = t_active;
    RCmd *c;
    if (!l || vc <= 0) return;
    c = push_cmd(l);
    if (!c) return;
    c->type = RC_DRAW_LINES;
    c->voff = push_bytes(&l->vtx, &l->vsize, &l->vcap, v, (size_t)vc * sizeof(TD5_D3DVertex));
    c->vc   = vc;
}

void td5_rcmd_replay(RCmdList *l)
{
    int i;
    if (!l) return;
    /* t_active must be NULL here so the platform fns run live, not re-record. */
    for (i = 0; i < l->ccount; i++) {
        RCmd *c = &l->cmds[i];
        switch (c->type) {
        case RC_PRESET:    td5_plat_render_set_preset((TD5_RenderPreset)c->a); break;
        case RC_FOG:       td5_plat_render_set_fog(c->a, c->u, c->f0, c->f1, c->f2); break;
        case RC_BIND_TEX:  td5_plat_render_bind_texture(c->a); break;
        case RC_BIND_PAGE: td5_render_bind_texture_page(c->a); break;
        case RC_VIEWPORT:  td5_plat_render_set_viewport(c->a, c->b, c->c, c->d); break;
        case RC_CLIP:      td5_plat_render_set_clip_rect(c->a, c->b, c->c, c->d); break;
        case RC_DRAW_TRIS:
            td5_plat_render_draw_tris((const TD5_D3DVertex *)(l->vtx + c->voff), c->vc,
                                      c->ic ? (const uint16_t *)(l->idx + c->ioff) : NULL, c->ic);
            break;
        case RC_DRAW_LINES:
            td5_plat_render_draw_lines((const TD5_D3DVertex *)(l->vtx + c->voff), c->vc);
            break;
        }
    }
}
