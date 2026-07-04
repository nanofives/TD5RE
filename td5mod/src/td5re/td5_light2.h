/**
 * td5_light2.h -- Lighting rework v2 (P0 infrastructure)
 *
 * Master mode knob + scene-lighting state for the reworked lighting system
 * (see LIGHTING_REWORK_PLAN.md). Phase 0 delivers:
 *
 *   Mode 0 (classic)  : byte-identical original path — grayscale per-vertex
 *                       lighting through the color LUT, dynamic lights with
 *                       no surface normals. Nothing new runs.
 *   Mode 1 (enhance)  : (a) COLORED zone lighting — the per-track zone table
 *                       carries authored RGB directional weights + RGB ambient
 *                       (weight_r/g/b, amb_r/g/b in td5_light_zones_table.inc)
 *                       that BOTH the original engine and the classic port
 *                       collapse to (r+g+b)/3 [orig @ 0x42E130 dir, @ 0x43E7B0
 *                       ambient]. Mode 1 keeps the per-channel ratios and
 *                       emits full ARGB vertex diffuse.
 *                       (b) G-BUFFER feed — world-space vertex normals +
 *                       material id ride the unused TD5_D3DVertex.specular
 *                       dword (COLOR1) into a second render target, and the
 *                       deferred dynamic-light pass (ps_light.hlsl) applies
 *                       proper N.L so headlights stop lighting back-facing
 *                       surfaces and leaking through geometry.
 *
 * RE BASIS: the classic path mirrors ComputeMeshVertexLighting @ 0x0043DDF0 /
 * SetTrackLightDirectionContribution @ 0x0042E130 / ComputeAverageDepth
 * @ 0x0043E7B0. Mode 1 is a deliberate port-only EXTENSION that reuses the
 * exact same zone data with the authored color no longer averaged away.
 *
 * DETERMINISM: render-side only. No sim state, no shared MSVC rand stream.
 */
#ifndef TD5_LIGHT2_H
#define TD5_LIGHT2_H

#define TD5_LIGHT2_CLASSIC 0
#define TD5_LIGHT2_ENHANCE 1

/* Driven by [Lighting] Mode / --LightingMode=N at startup (main.c). The env
 * knob TD5RE_LIGHT2_MODE overrides both for A/B testing. */
void td5_light2_set_mode(int mode);
int  td5_light2_mode(void);

/* Convenience: non-zero when any Mode>=1 feature (colored zones, G-buffer
 * vertex packing) should run. Hot paths read this once per mesh/frame. */
int  td5_light2_active(void);

/* ---- P2: screen-space ray-marched shadows ------------------------------ */
/* [Lighting] SunShadows / --SunShadows=N: 1 = fullscreen sun-shadow pass
 * (cars/walls shadow the road via depth-buffer ray marching). Only active
 * when Mode>=1. */
void td5_light2_set_sun_shadows(int on);
int  td5_light2_sun_shadows(void);

/* [Lighting] ShadowStrength / --ShadowStrength=N: max darkening in full
 * shadow, percent 0..100 (default 45). Scaled at runtime by the zone's
 * directional dominance so ambient-only zones (tunnels) cast nothing. */
void td5_light2_set_shadow_strength(int percent);
int  td5_light2_shadow_strength(void);

/* [Lighting] LightOcclusion / --LightOcclusion=N: 1 = dynamic lights
 * (headlights) march the depth buffer toward each light so they no longer
 * shine through cars/walls. Only active when Mode>=1. */
void td5_light2_set_light_occlusion(int on);
int  td5_light2_light_occlusion(void);

/* ---- P3: screen-space reflections -------------------------------------- */
/* [Lighting] Reflections / --Reflections=N: 1 = SSR pass (car paint, glass,
 * wet roads mirror the scene). Only active when Mode>=1. */
void td5_light2_set_reflections(int on);
int  td5_light2_reflections(void);

/* [Lighting] WetRoads / --WetRoads=N: 1 = rainy weather makes up-facing
 * roads reflective (needs Reflections=1). */
void td5_light2_set_wet_roads(int on);
int  td5_light2_wet_roads(void);

#endif /* TD5_LIGHT2_H */
