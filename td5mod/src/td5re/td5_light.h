/**
 * td5_light.h -- Dynamic light system (foundation)
 *
 * World-space positional POINT lights with distance falloff, consumed by the
 * software T&L lighting pass (td5_render_compute_vertex_lighting). Each light
 * brightens nearby geometry — the road, cars, props — so dark areas can be
 * illuminated dynamically. Headlights are the first concrete light source.
 *
 * RE BASIS / FAITHFULNESS
 *   The original TD5 engine (ComputeMeshVertexLighting @ 0x0043DDF0) lit each
 *   vertex with a 9-term dot product (3 DIRECTIONAL light vectors x the vertex
 *   normal) plus a scalar ambient, clamped to [0x40,0xFF], stored as the vertex
 *   diffuse colour. There are NO D3D fixed-function lights (no SetLight /
 *   LightEnable / CreateLight, no D3DRENDERSTATE_AMBIENT) — lighting is entirely
 *   CPU per-vertex. The original has NO headlights (only two rear grey
 *   0xFF909090 taillight billboards @ 0x004011C0) and NO positional/point lights
 *   at all — its whole vocabulary is "directional dots + scalar ambient".
 *
 *   This module is therefore a deliberate port-only EXTENSION built on the exact
 *   same per-vertex seam: it accumulates an additional N.L term with DISTANCE
 *   ATTENUATION into the same `intensity` scalar, before the original's clamp.
 *   That is the minimum addition needed for a light that "illuminates an area"
 *   (a moving headlight) rather than a fixed sun direction.
 *
 * COORDINATE SPACE
 *   Light positions are stored in the renderer's float "world units" (== the
 *   original 24.8 world fixed-point divided by 256, the same space as the
 *   camera position and an actor's render_pos). The per-vertex consumer
 *   transforms each light into the mesh's model space before the dot product,
 *   so the attenuation distance is measured correctly (rigid transform => the
 *   distance is preserved).
 */
#ifndef TD5_LIGHT_H
#define TD5_LIGHT_H

#include <stdint.h>

/* Hard cap on the number of simultaneous dynamic lights per frame. Sized for a
 * full grid of cars with a headlight pair each, plus headroom for future world
 * lights (street lamps, flares). */
#define TD5_LIGHT_MAX 32

typedef struct TD5_DynLight {
    float x, y, z;       /* world position (/256 world units, == render_pos space) */
    float range;         /* world units; contribution is zero at/beyond this radius */
    float intensity;     /* peak luminance bump at the lit surface (~0..255 scale)  */
    float r, g, b;       /* normalised colour 0..1 (reserved; v1 applies luminance) */
} TD5_DynLight;

/* ---- Master enable ----------------------------------------------------- */
/* Driven by [Lighting] Enabled / --Lighting=N at startup. When disabled the
 * registry stays empty and the per-vertex hook short-circuits to the original
 * (byte-faithful) lighting path. */
void td5_light_set_enabled(int on);
int  td5_light_enabled(void);

/* Enable/disable just the vehicle headlight emitter ([Lighting] Headlights /
 * --Headlights=N). Independent of the master enable so the registry can still
 * host other light sources with headlights off. */
void td5_light_set_headlights(int on);
int  td5_light_headlights_enabled(void);

/* ---- Per-frame lifecycle ----------------------------------------------- */
/* Clear the registry at the top of each rendered frame. Re-populate via the
 * emitters below BEFORE any viewport's lighting pass runs (lights are
 * world-space and shared across all split-screen panes). No-op when the master
 * enable is off. */
void td5_light_begin_frame(void);

/* Register one world-space point light. Silently ignored when the registry is
 * full or the master enable is off. range<=0 or intensity<=0 is ignored. */
void td5_light_add_point(float x, float y, float z,
                         float range, float intensity,
                         float r, float g, float b);

/* ---- Query (consumed by the renderer) ---------------------------------- */
int                 td5_light_count(void);
const TD5_DynLight *td5_light_get(int i);
const TD5_DynLight *td5_light_list(int *count);   /* NULL,0 when none/disabled */

/* ---- Emitters ---------------------------------------------------------- */
/* Vehicle headlights: for each live racer actor (and, when the traffic knob is
 * on, traffic actors), register a forward-mounted pair of point lights derived
 * from the actor's world position + body->world rotation matrix. Call once per
 * frame after td5_light_begin_frame(). No-op when headlights are disabled. */
void td5_light_emit_vehicle_headlights(void);

#endif /* TD5_LIGHT_H */
