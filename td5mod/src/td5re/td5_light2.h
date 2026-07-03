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

#endif /* TD5_LIGHT2_H */
