/**
 * td5_track_parser.c -- MODELS.DAT parsing (S6 module split, see REFACTOR_PLAN.md)
 *
 * Extracted verbatim from td5_track.c's "MODELS.DAT Parsing" section
 * (0x431190 ParseModelsDat + mesh-resource prep + additive-billboard
 * dimming). Pure code motion -- same statements, same order; shared
 * parser state/types come from td5_track_internal.h.
 */

#include "td5_track.h"
#include "td5_track_internal.h"
#include "td5_asset.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5re.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

#define LOG_TAG "track"

/* ========================================================================
 * MODELS.DAT Parsing (0x431190 ParseModelsDat)
 * [ARCH-DIVERGENCE @ 0x00431190] L5 promotion sweep audit (2026-05-18).
 *   Orig assumes a single fixed layout: DWORD[0]=entry_count,
 *   DWORD[1..count*2]=relocatable (offset, second_dword) pairs, then
 *   relocates entry pointers by adding param_1 base and recurses through
 *   PrepareMeshResource for each sub-mesh. Port detects three layout
 *   variants (A strict, A relaxed, B no-count-header) via DWORD
 *   heuristics, then auto-detects (offset, size) vs (size, offset)
 *   field order per-entry. Auto-detect is required because the
 *   shipping MODELS.DAT files use multiple layouts across tracks --
 *   orig binary just trusts the convention; port has to be defensive
 *   to handle the variation. Forward-direction lookup result is
 *   byte-equivalent to orig on stock content (Wave 5 audit).
 *
 * MODELS.DAT contains a sequence of mesh resources. Each entry begins
 * with a 4-byte size field followed by mesh data. The parser extracts
 * mesh headers and relocates internal pointers.
 * ======================================================================== */

int td5_track_parse_models_dat(const void *data, size_t size)
{
    const uint8_t *src;
    uint32_t raw_entry_count;
    int parsed_count = 0;
    uint32_t table_start_byte;   /* byte offset of entry table in file */

    if (!data || size < 8)
        return 0;

    free_models_dat_runtime();

    src = (const uint8_t *)data;
    s_models_blob = (uint8_t *)malloc(size);
    if (!s_models_blob)
        return 0;
    memcpy(s_models_blob, src, size);
    s_models_blob_size = size;

    /*
     * MODELS.DAT format (from Ghidra RE of SetupModelsDisplayList 0x431190):
     *
     *   DWORD[0] = entry_count (number of display list blocks)
     *   DWORD[1..count*2] = table of (offset, block_size) pairs, 8 bytes each
     *     offset: byte offset from file start to display list block
     *     block_size: byte size of the block
     *   [data blocks follow the table]
     *
     * The original lookup (0x431260) does: return *(table_base + span_index * 8)
     * where table_base = &DWORD[1].
     *
     * Heuristic to detect which format variant we have:
     *   - If DWORD[0] looks like a count (small number, and DWORD[1] == 4 + DWORD[0]*8),
     *     treat DWORD[0] as count with table at byte 4.
     *   - Otherwise, fall back to offset-based detection (DWORD[1] / 8 = entry count,
     *     table at byte 0).
     */
    {
        uint32_t dword0 = *(const uint32_t *)(const void *)(s_models_blob);
        uint32_t dword1 = *(const uint32_t *)(const void *)(s_models_blob + 4);

        /* Try format A (strict): DWORD[0] = count, DWORD[1] == 4 + count*8 */
        if (dword0 > 0 && dword0 < 10000 &&
            dword1 == 4 + dword0 * 8 &&
            (size_t)(4 + dword0 * 8) <= size) {
            raw_entry_count = dword0;
            table_start_byte = 4;
            TD5_LOG_I("track", "MODELS.DAT format A: count=%u table@4 first_block@%u",
                      raw_entry_count, dword1);
        }
        /* Try format A (relaxed): DWORD[0] looks like a count, DWORD[1] is the
         * first entry's offset (not necessarily == 4+count*8 due to padding).
         * Validate by checking that DWORD[1] points to a valid sub_mesh_count.
         * Must check BEFORE format B because format B misinterprets the count
         * header as entry[0] when DWORD[1] happens to be divisible by 8. */
        else if (dword0 > 0 && dword0 < 10000 && (size_t)(4 + dword0 * 8) <= size &&
                 dword1 >= 4 + dword0 * 8 && dword1 < size && dword1 + 4 <= size &&
                 *(const uint32_t *)(s_models_blob + dword1) >= 1 &&
                 *(const uint32_t *)(s_models_blob + dword1) <= 256) {
            raw_entry_count = dword0;
            table_start_byte = 4;
            TD5_LOG_I("track", "MODELS.DAT format A (relaxed): count=%u table@4 first_block@%u",
                      raw_entry_count, dword1);
        }
        /* Try format B: no count header, table at byte 0, count = DWORD[1]/8 */
        else if (dword1 > 0 && (dword1 & 7u) == 0 && dword1 <= size) {
            raw_entry_count = dword1 / 8u;
            table_start_byte = 0;
            TD5_LOG_I("track", "MODELS.DAT format B: count=%u table@0 first_block@%u",
                      raw_entry_count, dword1);
        }
        else {
            TD5_LOG_W("track", "MODELS.DAT: cannot determine format (dword0=%u dword1=%u size=%zu)",
                      dword0, dword1, size);
            free_models_dat_runtime();
            return 0;
        }
    }

    if (raw_entry_count == 0) {
        free_models_dat_runtime();
        return 0;
    }

    s_models_entry_offsets = (uint32_t *)calloc(raw_entry_count, sizeof(uint32_t));
    s_models_entry_sizes = (uint32_t *)calloc(raw_entry_count, sizeof(uint32_t));
    if (!s_models_entry_offsets || !s_models_entry_sizes) {
        free_models_dat_runtime();
        return 0;
    }

    for (uint32_t i = 0; i < raw_entry_count; i++) {
        uint32_t tbl_byte = table_start_byte + i * 8u;
        uint32_t entry_offset, entry_size;
        const uint8_t *entry_base;
        uint32_t sub_mesh_count;

        if (tbl_byte + 8 > size)
            break;

        {
            uint32_t f0 = *(const uint32_t *)(const void *)(s_models_blob + tbl_byte);
            uint32_t f1 = *(const uint32_t *)(const void *)(s_models_blob + tbl_byte + 4);

            /* Auto-detect field order: one field is the block offset (should be
             * >= table end), the other is the block size (smaller).
             * Check which field points to a valid sub_mesh_count (1..256). */
            uint32_t table_end = table_start_byte + raw_entry_count * 8u;
            int f0_is_offset = (f0 >= table_end && f0 < size &&
                                f0 + 4 <= size &&
                                *(const uint32_t *)(s_models_blob + f0) >= 1 &&
                                *(const uint32_t *)(s_models_blob + f0) <= 256);
            int f1_is_offset = (f1 >= table_end && f1 < size &&
                                f1 + 4 <= size &&
                                *(const uint32_t *)(s_models_blob + f1) >= 1 &&
                                *(const uint32_t *)(s_models_blob + f1) <= 256);

            if (f0_is_offset && !f1_is_offset) {
                entry_offset = f0; entry_size = f1;  /* [offset, size] */
            } else if (f1_is_offset && !f0_is_offset) {
                entry_offset = f1; entry_size = f0;  /* [size, offset] */
            } else if (f0_is_offset && f1_is_offset) {
                /* [BUGFIX 2026-05-26 invisible-road] Both fields parse as
                 * valid offsets — rare collision where the size field's
                 * value happens to point to a dword in [1,256] (e.g. a
                 * float bit-pattern or a vertex-count from some other
                 * block). Prior code picked min(f0,f1) which selected the
                 * false positive and pointed the renderer at junk geometry
                 * → invisible road segments. Real-world hits: L016 entry
                 * 314 (Edinburgh spans 1256-59), L017 entries 41/485/620
                 * (Blue Ridge), L002 entries 134/162/393/436 (Sydney),
                 * L014 entry 679. Always prefer f0: scan of all shipped
                 * Pitbull MODELS.DAT (L001-L039) shows f0 is 100% monotonic
                 * and 100% valid; zero entries actually use [size,offset]. */
                entry_offset = f0; entry_size = f1;
            } else {
                entry_offset = f0; entry_size = f1;  /* fallback */
            }
        }

        if (entry_offset == 0 || entry_offset >= size)
            continue;

        /* Compute block size from next entry's offset if available,
         * as entry_size may not be reliable in all format variants */
        if (entry_size == 0 || (size_t)entry_offset + (size_t)entry_size > size) {
            /* Estimate size from next entry or end of file */
            uint32_t next_off = (uint32_t)size;
            if (i + 1 < raw_entry_count) {
                uint32_t next_tbl = table_start_byte + (i + 1) * 8u;
                if (next_tbl + 8 <= size) {
                    /* Pick whichever field of the next entry looks like a
                     * plausible offset (> current offset, within file). */
                    uint32_t nf0 = *(const uint32_t *)(const void *)(s_models_blob + next_tbl);
                    uint32_t nf1 = *(const uint32_t *)(const void *)(s_models_blob + next_tbl + 4);
                    uint32_t candidate = (nf0 > entry_offset && nf0 <= size) ? nf0 :
                                         (nf1 > entry_offset && nf1 <= size) ? nf1 : (uint32_t)size;
                    if (candidate > entry_offset && candidate <= size)
                        next_off = candidate;
                }
            }
            entry_size = next_off - entry_offset;
        }

        if (entry_size < 8 || (size_t)entry_offset + (size_t)entry_size > size)
            continue;

        entry_base = s_models_blob + entry_offset;
        sub_mesh_count = *(const uint32_t *)(const void *)entry_base;
        if (sub_mesh_count == 0 || sub_mesh_count > 256)
            continue;
        if (entry_size < 4u + sub_mesh_count * 4u)
            continue;

        /* Store at raw table index i (not compacted display_list_count).
         * The original lookup (0x431260) does: table_base + span_index * 8,
         * so entry[i] must correspond to span i — not the i-th valid entry.
         * Invalid entries stay 0 from calloc → lookup returns NULL for them. */
        s_models_entry_offsets[i] = entry_offset;
        s_models_entry_sizes[i] = entry_size;

        {
            const TD5_TrackRawMeshHeader *first_mesh;
            /* Use i (raw index) for the header lookup since offsets are now
             * stored at their true table position. */
            s_models_display_list_count = i + 1;
            if (parsed_count < MODELS_DAT_MAX_ENTRIES &&
                get_display_list_first_mesh_header(i, &first_mesh)) {
                TD5_ModelsDatEntry *dst = &s_models[parsed_count++];
                dst->mesh_ptr = (void *)(uintptr_t)first_mesh;
                dst->texture_page_id = (int32_t)first_mesh->texture_page_id;
                dst->bounding_radius = first_mesh->bounding_radius;
            }
        }
    }

    s_models_display_list_count = raw_entry_count;
    s_model_count = parsed_count;

    /* Relocate mesh pointers within each display list block.
     * Each block is: [sub_mesh_count][mesh_offset_0][mesh_offset_1]...
     * mesh_offset values are byte offsets from block start → convert to
     * absolute pointers. Then relocate internal MeshHeader fields. */
    for (int dl = 0; dl < s_models_display_list_count; dl++) {
        if (s_models_entry_offsets[dl] == 0) continue;  /* skip invalid entries */
        uint8_t *block_base = s_models_blob + s_models_entry_offsets[dl];
        uint32_t block_size = s_models_entry_sizes[dl];
        uint32_t sub_count = *(uint32_t *)block_base;

        if (sub_count == 0 || sub_count > 256 || block_size < 4u + sub_count * 4u)
            continue;

        for (uint32_t j = 0; j < sub_count; j++) {
            uint32_t *slot = (uint32_t *)(block_base + 4 + j * 4);
            uint32_t mesh_off = *slot;

            if (mesh_off == 0) continue;

            /* Convert relative offset to absolute pointer.
             * Try block-relative first, then blob-relative for offsets
             * that exceed the block boundary (some MODELS.DAT entries
             * store global blob offsets instead of block-local offsets). */
            TD5_MeshHeader *mesh;
            if (mesh_off < block_size && mesh_off + sizeof(TD5_MeshHeader) <= block_size) {
                mesh = (TD5_MeshHeader *)(block_base + mesh_off);
            } else if (mesh_off < s_models_blob_size &&
                       mesh_off + sizeof(TD5_MeshHeader) <= s_models_blob_size) {
                mesh = (TD5_MeshHeader *)(s_models_blob + mesh_off);
            } else {
                *slot = 0;
                continue;
            }
            *slot = (uint32_t)(uintptr_t)mesh;

            /* Validate mesh fields before relocation.
             * Allow command_count==0 or vertex_count==0 — the original game
             * has placeholder/empty meshes that the renderer simply skips. */
            if (mesh->command_count < 0 || mesh->command_count > 4096 ||
                mesh->total_vertex_count < 0 || mesh->total_vertex_count > 65536) {
                static int s_vfail_log = 0;
                if (s_vfail_log < 5) {
                    TD5_LOG_W("track", "MODELS.DAT mesh validation fail: dl=%d slot=%d "
                              "cmds=%d verts=%d off=0x%x",
                              dl, j, mesh->command_count, mesh->total_vertex_count, mesh_off);
                    s_vfail_log++;
                }
                *slot = 0;
                continue;
            }

            /* Validate internal offsets stay within the models blob before
             * relocating.  A wild commands_offset or vertices_offset is the
             * most common crash vector for tracks with unusual MODELS.DAT.
             * Offsets are relative to the mesh header start. */
            {
                uintptr_t mesh_abs = (uintptr_t)mesh;
                uintptr_t blob_end = (uintptr_t)s_models_blob + s_models_blob_size;
                uint32_t cmd_off = mesh->commands_offset;
                uint32_t vtx_off = mesh->vertices_offset;

                if (cmd_off != 0 && mesh_abs + cmd_off >= blob_end) {
                    *slot = 0;
                    continue;
                }
                if (vtx_off != 0 && mesh_abs + vtx_off >= blob_end) {
                    *slot = 0;
                    continue;
                }
            }

            /* Relocate commands/vertices/normals offsets within mesh */
            td5_track_prepare_mesh_resource(mesh);
        }
    }

    /* Post-relocation validation: log bad blocks but do NOT disable all
     * display lists.  The per-mesh validation in td5_render_span_display_list
     * already skips individual bad meshes safely.  The old 25% threshold was
     * disabling tracks that were 72% valid, leaving no visible geometry. */
    {
        int bad_blocks = 0;
        for (int dl = 0; dl < s_models_display_list_count; dl++) {
            if (s_models_entry_offsets[dl] == 0) continue;  /* skip invalid entries */
            uint8_t *blk = s_models_blob + s_models_entry_offsets[dl];
            uint32_t sc = *(uint32_t *)blk;
            if (sc == 0 || sc > 256) { bad_blocks++; continue; }
            for (uint32_t j = 0; j < sc; j++) {
                uint32_t ptr_val = *(uint32_t *)(blk + 4 + j * 4);
                if (ptr_val == 0) continue;
                TD5_MeshHeader *m = (TD5_MeshHeader *)(uintptr_t)ptr_val;
                if ((uintptr_t)m < 0x10000u ||
                    !td5_track_is_ptr_in_blob(m, sizeof(TD5_MeshHeader)) ||
                    m->command_count < 0 || m->command_count > 4096 ||
                    m->total_vertex_count < 0 || m->total_vertex_count > 65536) {
                    static int s_post_fail_log = 0;
                    if (s_post_fail_log < 10) {
                        TD5_LOG_W("track",
                            "post-reloc fail: dl=%d j=%d ptr=0x%08X in_blob=%d "
                            "cmds=%d verts=%d cmd_off=0x%08X vtx_off=0x%08X",
                            dl, (int)j, (unsigned)ptr_val,
                            td5_track_is_ptr_in_blob(m, sizeof(TD5_MeshHeader)),
                            ((uintptr_t)m >= 0x10000u) ? m->command_count : -1,
                            ((uintptr_t)m >= 0x10000u) ? m->total_vertex_count : -1,
                            ((uintptr_t)m >= 0x10000u) ? m->commands_offset : 0,
                            ((uintptr_t)m >= 0x10000u) ? m->vertices_offset : 0);
                        s_post_fail_log++;
                    }
                    *(uint32_t *)(blk + 4 + j * 4) = 0;
                    bad_blocks++;
                }
            }
        }
        if (bad_blocks > 0) {
            TD5_LOG_W("track",
                "MODELS.DAT: %d/%d blocks had bad meshes (zeroed, not disabled)",
                bad_blocks, s_models_display_list_count);
        }
    }

    if (s_models_display_list_count > 0 && s_span_count > 0)
        rebuild_span_display_list_mapping();

    TD5_LOG_I("track", "MODELS.DAT: %d/%u display lists, %d model entries, blob=%zuB",
              s_models_display_list_count, raw_entry_count, s_model_count, size);

    return parsed_count;
}

/**
 * PrepareMeshResource (0x40AC00).
 * Relocates internal offset fields in a mesh header to absolute pointers.
 * The commands_offset, vertices_offset, and normals_offset fields are
 * converted from byte-offsets-from-header-start to absolute pointers.
 *
 * L5 promotion sweep audit (2026-05-18) — partial L5 + ARCH-DIVERGENCE.
 *
 * Pointer relocation (header +0x2C/+0x30/+0x34) is byte-faithful with
 * orig 0x0040AC00 lines `*(int*)(param_1+0x2C) = ... + param_1;` etc.
 * [CONFIRMED @ 0x0040AC0F-0x0040AC32 disasm.]
 *
 * The port intentionally OMITS three secondary passes from orig:
 *
 * 1. Per-vertex UV clamp + half-texel inset
 *    Orig walks every vertex (stride 0x2C, count = u_vertex_count*3 +
 *    quad_count*4) and clamps `iVar8+0x1C` (u), `iVar8+0x20` (v) to
 *    [0.0, 1.0], then transforms `u = u * 0.984375f + 0.0078125f`
 *    (and likewise for v). The scale/bias come from
 *    [DAT_0045D61C=0x3C000000=1/128] and [DAT_0045D620=0x3F7C0000=255/256],
 *    a classic D3D3 nearest-neighbor half-texel inset to prevent
 *    sampling neighboring texels at u or v exactly 0 or 1.
 *    [CONFIRMED @ 0x0040AC4C-0x0040AC91, constants dumped 2026-05-18.]
 *
 *    [ARCH-DIVERGENCE: The D3D11 wrapper samples with bilinear filtering
 *    and clamp/border addressing. Half-texel inset is unnecessary because
 *    the GPU sampler handles edge sampling correctly — applying it would
 *    actually skew texture coordinates by 0.78% and produce a 1-pixel
 *    seam on every UV island edge. UVs are passed through as-is.]
 *
 * 2. Per-mesh-batch GetTextureSlotStatus primitive-type tag
 *    Orig calls GetTextureSlotStatus(mesh->texture_page_id) once per
 *    batch and writes `psVar7[0] = (-(loaded) & 3) + 1` — encodes the
 *    D3D3 primitive opcode (TLVERTEX vs LVERTEX vs vertex) based on
 *    whether the texture page is resident.
 *    [CONFIRMED @ 0x0040ACA4-0x0040ACBA disasm.]
 *
 *    [ARCH-DIVERGENCE: The port has no D3D3 opcode-rewrite pass — the
 *    wrapper backend uses a single immediate-mode vertex format selected
 *    at the draw call site. Documented separately in td5_render.c near
 *    "The source port has no PrepareMeshResource opcode-rewrite pass".
 *    Additive billboard ordering is recovered via the deferred-additive
 *    flush there, not via per-batch opcode tagging.]
 *
 * 3. Per-face culling-flag prepass (iVar5+0xC)
 *    Orig walks faces (tris @ stride 0x30, quads @ stride 0x40) and
 *    writes `face_flags = (any vertex.y <= 0.0366) ? 0 : 1`. This is
 *    a screen-space-distance backface/cull cache populated at load
 *    time, then read per-frame at draw time to skip degenerate faces.
 *    [CONFIRMED @ 0x0040ACDD-0x0040AD9A disasm; threshold
 *    [DAT_0045D618=0x3D15FEDA=~0.0366].]
 *
 *    [ARCH-DIVERGENCE: Port culls per-triangle in the software-transform
 *    path (see clip_emit_tris in td5_render.c) using screen-space area
 *    sign and near-plane Z, computed each frame from the CURRENT camera
 *    transform. Caching a load-time cull flag against a pre-transform
 *    vertex-y threshold would be incorrect for a moving camera.]
 *
 * Net behaviour: the port's prepare_mesh_resource does the ONE step
 * (pointer relocation) that matters for downstream parity; the omitted
 * passes are intentional D3D3->D3D11 / immediate->software-transform
 * architectural simplifications, not bugs.
 */
void td5_track_prepare_mesh_resource(TD5_MeshHeader *mesh)
{
    uint8_t *base;

    if (!mesh)
        return;

    base = (uint8_t *)mesh;

    /* Relocate offsets to absolute pointers.
     * The original stores these as uint32 offsets relative to the mesh
     * header start. We convert them to pointers (stored back as uint32
     * for the original 32-bit engine). In the source port we store them
     * as uintptr_t-compatible values. */
    if (mesh->commands_offset != 0)
        mesh->commands_offset = (uint32_t)(uintptr_t)(base + mesh->commands_offset);
    if (mesh->vertices_offset != 0)
        mesh->vertices_offset = (uint32_t)(uintptr_t)(base + mesh->vertices_offset);
    if (mesh->normals_offset != 0)
        mesh->normals_offset = (uint32_t)(uintptr_t)(base + mesh->normals_offset);

    /* Note: per-vertex diffuse dim for additive billboards lives in a
     * separate post-pass (td5_track_dim_additive_billboard_meshes) called
     * AFTER track textures load. At this point the page transparency
     * table is still empty and we can't tell which billboard tags (1/2)
     * correspond to real lights (type-3 page) vs normal trees/signs
     * (type-1 alpha-keyed). Dimming all of them would wash out trees. */
}

void td5_track_dim_additive_billboard_meshes(void)
{
    /* Walk every parsed MODELS.DAT display list, find billboard meshes
     * (mesh header +0x02 == 1 || 2) whose FIRST command renders through
     * a type-3 (additive) texture page, and halve their per-vertex
     * diffuse.
     *
     * Why: the asset authors baked per-vertex intensity around 0xA0 for
     * streetlight quads in a 16bpp R5G6B5 + ONE/ONE additive pipeline.
     * At 32bpp the same values saturate the framebuffer because the
     * additive sum has more headroom before the 0xFF clamp. Halving
     * pulls the additive result back into the CRT-era perceptual range.
     *
     * One-shot per track load (call AFTER td5_asset_load_track_textures
     * so the transparency table is populated). Regular alpha-keyed
     * billboards (trees, signs — type-1 pages) are left untouched. */
    int dimmed = 0;
    int total_bb = 0;

    for (int dl = 0; dl < s_models_display_list_count; dl++) {
        if (s_models_entry_offsets[dl] == 0) continue;
        uint8_t *block_base = s_models_blob + s_models_entry_offsets[dl];
        uint32_t sub_count = *(const uint32_t *)block_base;
        if (sub_count == 0 || sub_count > 256) continue;

        for (uint32_t j = 0; j < sub_count; j++) {
            uint32_t mesh_ptr_val = *(const uint32_t *)(block_base + 4 + j * 4);
            if (mesh_ptr_val == 0) continue;

            TD5_MeshHeader *mesh = (TD5_MeshHeader *)(uintptr_t)mesh_ptr_val;
            if (!td5_track_is_ptr_in_blob(mesh, sizeof(TD5_MeshHeader)))
                continue;
            if (mesh->texture_page_id != 1 && mesh->texture_page_id != 2)
                continue;
            if (mesh->commands_offset == 0 || mesh->vertices_offset == 0)
                continue;
            if (mesh->command_count <= 0 || mesh->total_vertex_count <= 0)
                continue;
            if (mesh->total_vertex_count > 65536)
                continue;

            total_bb++;

            /* Check the first command's texture page — if not type-3 we
             * leave the mesh alone (that's a tree/sign, not a light). */
            const TD5_PrimitiveCmd *cmd0 =
                (const TD5_PrimitiveCmd *)(uintptr_t)mesh->commands_offset;
            if (td5_asset_get_page_transparency(cmd0->texture_page_id) != 3)
                continue;

            /* Scale per-vertex RGB to ~25% of disk value. Disk-baked
             * intensity is 0xA0 (160, 63% of full). Halving to 0x50 (80,
             * 31%) still read slightly too bright on a 32bpp
             * framebuffer; scaling by 3/8 → ~0x3C (60, 24%) lands in the
             * CRT-era additive perceptual range. */
            TD5_MeshVertex *bb_v =
                (TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;
            for (int vi = 0; vi < mesh->total_vertex_count; vi++) {
                uint32_t c = bb_v[vi].lighting;
                uint32_t a = c & 0xFF000000u;
                uint32_t r = (c >> 16) & 0xFFu;
                uint32_t g = (c >>  8) & 0xFFu;
                uint32_t b =  c        & 0xFFu;
                r = (r * 3u) >> 3; g = (g * 3u) >> 3; b = (b * 3u) >> 3;
                bb_v[vi].lighting = a | (r << 16) | (g << 8) | b;
            }
            dimmed++;
        }
    }

    TD5_LOG_I("track",
        "additive billboard dim: %d/%d billboard meshes had type-3 pages "
        "(halved per-vertex diffuse)", dimmed, total_bb);
}
