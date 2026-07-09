#!/usr/bin/env python3
"""Phase 5(a) class-level ARCH-DIVERGENCE manifests for the 33 remaining L4
entries that fit clear class patterns."""
from pathlib import Path


def emit_block(title, short_tag, rationale, entries):
    lines = ['/* ============================================================',
             f' * [ARCH-DIVERGENCE: {title}] Phase 5(a) class manifest (2026-05-21)',
             ' *']
    words = rationale.split()
    cur = ' *'
    for w in words:
        if len(cur) + 1 + len(w) > 74:
            lines.append(cur)
            cur = ' * ' + w
        else:
            cur = (cur + ' ' + w) if cur != ' *' else (cur + ' ' + w)
    if cur.strip() != '*':
        lines.append(cur)
    lines.append(' *')
    max_name = max(len(n) for _, n in entries)
    for addr, name in entries:
        lines.append(f' *   {addr}  {name.ljust(max_name)}  [ARCH-DIVERGENCE: {short_tag}]')
    lines.append(' */')
    return '\n'.join(lines)


# td5_render.c additions
fade_block = emit_block(
    'cross-fade / fade-overlay collapse',
    'CrossFade',
    ("Orig's CrossFade16BitSurfaces (two entry points 0x0040CDC0 + 0x0040D190) "
     "iterates 16-bit DDraw surfaces scanline-by-scanline, blending pixel-pairs "
     "across a transition. AdvanceCrossFadeTransition (0x0040D120) advances the "
     "per-pixel mix factor. The port replaces all three with a single full-screen "
     "quad pass under D3D11 backbuffer at td5_render_crossfade_surfaces "
     "(td5_render.c:~3666). Same conceptual blend curve; the pixel-walk is gone "
     "because D3D11 doesn't expose lockable surfaces."),
    [('0x0040CDC0', 'CrossFade16BitSurfaces (variant 1)'),
     ('0x0040D120', 'AdvanceCrossFadeTransition'),
     ('0x0040D190', 'CrossFade16BitSurfaces (variant 2)')],
)

mesh_xform_block = emit_block(
    'mesh transform / projection helpers',
    'MeshXform',
    ("Orig has multiple per-mesh transform helpers operating on 12-float 3x4 "
     "matrices stored in DDraw-era globals. ApplyMeshRenderBasisFromTransform "
     "and TransformShortVectorToView are fp16 view-space transforms; "
     "TransformTriangleByRenderMatrix is per-triangle projection-pipeline "
     "transform; ApplyMeshProjectionEffect generates water/envmap UVs. The port "
     "routes all of these through s_render_transform.m + inline mat3x3 helpers; "
     "per-helper functions are folded into td5_render_transform_mesh_vertices "
     "and dispatch_projected_* callers."),
    [('0x0042D880', 'ApplyMeshRenderBasisFromTransform'),
     ('0x0042E3D0', 'TransformShortVectorToView'),
     ('0x0042E560', 'TransformTriangleByRenderMatrix'),
     ('0x0043DEC0', 'ApplyMeshProjectionEffect')],
)

depth_sort_block = emit_block(
    'depth-sort bucket management',
    'DepthSort',
    ("Orig's depth-sort bucket system uses raw-heap scratch buffers and global "
     "state (DAT_004af268 / DAT_004af278) for the projected-primitive linked "
     "lists. Port consolidates this into typed struct arrays (one "
     "TD5_RenderBucketEntry per slot) plus inline reset semantics inside "
     "td5_render_flush_projected_buckets and td5_render_init. Four orig helper "
     "functions (Reset/Initialize/Insert/Flush) fold into the consolidated "
     "init+flush path; semantically equivalent (same 4096-bucket inverse-Z "
     "layout) without the raw-byte scratch interface."),
    [('0x0043E2C0', 'ResetProjectedPrimitiveWorkBuffer'),
     ('0x0043E2F0', 'FlushProjectedPrimitiveBuckets'),
     ('0x0043E3B0', 'InsertBillboardIntoDepthSortBuckets'),
     ('0x0043E5F0', 'InitializeProjectedPrimitiveBuckets')],
)

track_light_block = emit_block(
    'per-segment track lighting',
    'TrackLight',
    ("Orig blends per-segment ambient light entries during span traversal "
     "(BlendTrackLightEntryFromStart/End) and per-actor track-light state "
     "snapshot (UpdateActorTrackLightState). All three fold in port into "
     "td5_render_apply_track_lighting (td5_render.c:~2102) called BEFORE "
     "compute_vertex_lighting in the per-actor vehicle dispatch path (mirrors "
     "ApplyTrackLightingForVehicleSegment @ 0x00430150). Output is same "
     "s_light_dirs[] / s_ambient_intensity globals; orig's linked-list blend "
     "at segment boundary collapses into a single per-actor lookup."),
    [('0x0040CD10', 'UpdateActorTrackLightState'),
     ('0x0042FE20', 'BlendTrackLightEntryFromStart'),
     ('0x0042FFC0', 'BlendTrackLightEntryFromEnd')],
)

# td5_frontend.c additions
surface_blit_block = emit_block(
    'frontend secondary-surface blit collapse',
    'SurfBlit',
    ("Orig has secondary frontend surface management (Blit/Present/Fill helpers "
     "for the primary+secondary 16-bit DDraw surfaces) that walks rect lists "
     "and blits via Lock+memcpy+Unlock or vtbl[0x1c] Blt. Port consolidates "
     "all six entry points into the D3D11 backbuffer + immediate-mode quad "
     "batch pipeline (fe_draw_surface_rect / td5_plat_present). Source-port "
     "architecture has no lockable primary/secondary surface model, so these "
     "orig functions are structurally collapsed."),
    [('0x00424C10', 'PresentSecondaryFrontendRectViaCopy'),
     ('0x00424C50', 'BlitSecondaryFrontendRectToPrimary'),
     ('0x00424CF0', 'PresentSecondaryFrontendRect'),
     ('0x00424D40', 'PresentPrimaryFrontendRect'),
     ('0x00424D90', 'FillPrimaryFrontendScanline'),
     ('0x00424E40', 'InitializeFrontendPresentationState')],
)

font_block = emit_block(
    'frontend font-string render collapse',
    'FontStr',
    ("Orig MeasureOrDrawFrontendFontString / DrawFrontendFontStringToSurface / "
     "DrawFrontendSmallFontStringToSurface walk per-glyph 8x8 bitmaps and blit "
     "to a DDraw surface. Port replaces with fe_draw_text consuming a "
     "glyph-strip atlas (snkmouse.tga-style fonts at td5_frontend.c:~488). "
     "Source-port has no scanline-blit font system; all three entries fold into "
     "the glyph-strip path."),
    [('0x00412D50', 'MeasureOrDrawFrontendFontString'),
     ('0x00424470', 'DrawFrontendFontStringToSurface'),
     ('0x00424660', 'DrawFrontendSmallFontStringToSurface')],
)

display_block = emit_block(
    'display-mode slot lookup',
    'DispMode',
    ("Orig SelectConfiguredDisplayModeSlot indexes a DXDraw-internal "
     "dd_exref+0x34 mode-table; port uses td5_plat_enum_display_modes (DXGI) "
     "and selects by index against the platform-enumerated list. Both lookups "
     "produce equivalent (width, height, bpp) tuples; the slot-index contract "
     "is preserved."),
    [('0x0040B170', 'SelectConfiguredDisplayModeSlot')],
)

# td5_hud.c addition
hud_glyph_block = emit_block(
    'HUD glyph strip render',
    'HudGlyph',
    ("Orig RenderPositionerGlyphStrip rasterizes 8x8 glyphs into a DDraw HUD "
     "surface for the positioner/race-position overlay. Port uses the unified "
     "glyph-strip atlas + fe_draw_text helper (td5_hud references "
     "td5_frontend's font system). Source-port collapses the HUD-specific "
     "glyph rasterizer into the shared font path."),
    [('0x00414F40', 'RenderPositionerGlyphStrip')],
)

# td5_vfx.c addition
particle_block = emit_block(
    'particle system pipeline collapse',
    'Particle',
    ("Orig has 8 entry points for per-race particle/streak system: Initialize "
     "/ Project / Draw / Update / Spawn / InitializeWeatherOverlay / "
     "UpdateAmbientParticleDensity / RenderAmbientParticleStreaks. All are "
     "ported in td5_vfx.c via the unified td5_vfx_* family (init_race_particles, "
     "update_race_particles, render_race_particles). Orig's per-step projection "
     "into camera-space + sprite-quad emit gets unified into a single D3D11 "
     "quad-batch pipeline; per-entry timing semantics preserved but parallel "
     "globals (DAT_004ab0e0 etc.) consolidated into td5_vfx.c statics."),
    [('0x00429510', 'InitializeRaceParticleSystem'),
     ('0x00429690', 'ProjectRaceParticlesToView'),
     ('0x00429720', 'DrawRaceParticleEffects'),
     ('0x00429790', 'UpdateRaceParticleEffects'),
     ('0x0042A6B0', 'SpawnAmbientParticleStreak'),
     ('0x00446240', 'InitializeWeatherOverlayParticles'),
     ('0x004464B0', 'UpdateAmbientParticleDensityForSegment'),
     ('0x00446560', 'RenderAmbientParticleStreaks')],
)

# Apply each footer
appends = {
    'td5mod/src/td5re/td5_render.c': ('\n\n' + fade_block + '\n\n' + mesh_xform_block +
                                        '\n\n' + depth_sort_block + '\n\n' + track_light_block + '\n'),
    'td5mod/src/td5re/td5_frontend.c': ('\n\n' + surface_blit_block + '\n\n' + font_block +
                                          '\n\n' + display_block + '\n'),
    'td5mod/src/td5re/td5_hud.c': '\n\n' + hud_glyph_block + '\n',
    'td5mod/src/td5re/td5_vfx.c': '\n\n' + particle_block + '\n',
}

count = 0
for path_str, content in appends.items():
    p = Path(path_str)
    existing = p.read_text(encoding='utf-8', errors='replace')
    if 'Phase 5(a) class manifest' in existing:
        print(f'SKIP — already has 5(a) manifest: {path_str}')
        continue
    if not existing.endswith('\n'):
        existing += '\n'
    p.write_text(existing + content, encoding='utf-8', newline='')
    n_blocks = content.count('Phase 5(a) class manifest')
    n_total = content.count('[ARCH-DIVERGENCE:')
    n_entries = n_total - n_blocks
    count += n_entries
    print(f'OK — {path_str}: {n_blocks} blocks, {n_entries} entries')

print(f'\nTotal entries promoted via 5(a): {count}')
