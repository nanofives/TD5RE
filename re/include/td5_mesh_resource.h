/**
 * td5_mesh_resource.h -- PRR Mesh Resource structures for Test Drive 5
 *
 * The PRR format is the unified mesh resource used for ALL 3D objects:
 * track models (MODELS.DAT), vehicles (himodel.dat), sky dome (SKY.PRR),
 * and traffic (model%d.prr).
 *
 * Derived from reverse engineering of TD5_d3d.exe.
 * See re/analysis/3d-asset-formats.md for full documentation.
 */

#ifndef TD5_MESH_RESOURCE_H
#define TD5_MESH_RESOURCE_H

#include <stddef.h>
#include <stdint.h>

/* ======================================================================
 * Constants
 * ====================================================================== */

#define TD5_FIXED_POINT_SCALE    0.00390625f   /* 1/256, world coord -> float */
#define TD5_UV_SCALE             0.984375f     /* 63/64, half-texel inset scale */
#define TD5_UV_OFFSET            0.0078125f    /* 0.5/64, half-texel inset bias */
#define TD5_LIGHTING_MIN         0x40           /* minimum vertex intensity */
#define TD5_LIGHTING_MAX         0xFF           /* maximum vertex intensity */
#define TD5_BACKFACE_THRESHOLD   0.03662f       /* normal Y below this -> face hidden */
#define TD5_MAX_TRANSLUCENT_BATCHES  510        /* 0x1FE */
#define TD5_D3D_FVF             0x1C4           /* XYZRHW|DIFFUSE|SPECULAR|TEX1 */

/* Primitive dispatch indices (into PTR_EmitTranslucentTriangleStrip at 0x473B9C) */
#define TD5_DISPATCH_TRISTRIP           0   /* EmitTranslucentTriangleStrip (0x431750) */
#define TD5_DISPATCH_TRISTRIP_ALT       1   /* same as 0 */
#define TD5_DISPATCH_PROJECTED_TRI      2   /* SubmitProjectedTrianglePrimitive (0x4316F0) */
#define TD5_DISPATCH_PROJECTED_QUAD     3   /* SubmitProjectedQuadPrimitive (0x431690) */
#define TD5_DISPATCH_BILLBOARD          4   /* InsertBillboardIntoDepthSortBuckets (0x43E3B0) */
#define TD5_DISPATCH_TRISTRIP_DIRECT    5   /* EmitTranslucentTriangleStripDirect (0x431730) */
#define TD5_DISPATCH_QUAD_DIRECT        6   /* EmitTranslucentQuadDirect (0x4316D0) */

/* ======================================================================
 * Mesh Resource Header (PRR format, >= 0x38 bytes)
 *
 * Used for track models, vehicles, sky, traffic.
 * Offsets +0x2C, +0x30, +0x34 are relative offsets in the file,
 * relocated to absolute pointers by PrepareMeshResource (0x40AC00).
 * ====================================================================== */

typedef struct MeshResourceHeader {
    int16_t  render_type;           /* +0x00: primitive dispatch index (0-6) */
    int16_t  texture_page_id;       /* +0x02: texture slot index */
    int32_t  command_count;         /* +0x04: number of sub-batch commands */
    int32_t  total_vertex_count;    /* +0x08: total vertices across all commands */
    float    bounding_radius;       /* +0x0C: bounding sphere radius (world units) */
    float    bounding_center_x;     /* +0x10: bounding sphere center X (fixed-point) */
    float    bounding_center_y;     /* +0x14: bounding sphere center Y */
    float    bounding_center_z;     /* +0x18: bounding sphere center Z */
    float    origin_x;              /* +0x1C: model origin X (fixed-point * 1/256) */
    float    origin_y;              /* +0x20: model origin Y */
    float    origin_z;              /* +0x24: model origin Z */
    uint32_t reserved_28;           /* +0x28: padding */
    uint32_t commands_offset;       /* +0x2C: -> PrimitiveCommand array (relocated) */
    uint32_t vertices_offset;       /* +0x30: -> MeshVertex array (relocated) */
    uint32_t normals_offset;        /* +0x34: -> FaceNormal array (0 = none, relocated) */
} MeshResourceHeader;

/* ======================================================================
 * Primitive Command (16 bytes, 0x10)
 *
 * One per sub-batch in the command list. Each command references
 * (tri_count * 3 + quad_count * 4) vertices in the vertex buffer.
 * ====================================================================== */

typedef struct PrimitiveCommand {
    int16_t  dispatch_type;         /* +0x00: indexes into dispatch table (0-6) */
    int16_t  texture_page_id;       /* +0x02: texture slot (overrides header) */
    int32_t  reserved_04;           /* +0x04: param / padding */
    uint16_t triangle_count;        /* +0x08: number of triangles (3 verts each) */
    uint16_t quad_count;            /* +0x0A: number of quads (4 verts each) */
    uint32_t vertex_data_ptr;       /* +0x0C: set by PrepareMeshResource to absolute ptr */
} PrimitiveCommand;

/* ======================================================================
 * Mesh Vertex (44 bytes, 0x2C, stride = 11 floats)
 *
 * Model-space fields (+0x00..+0x08) and texture coords (+0x1C..+0x28)
 * are stored in the file. Runtime fields (+0x0C..+0x18) are computed
 * per-frame by the transform and lighting pipeline.
 * ====================================================================== */

typedef struct MeshVertex {
    float    pos_x;                 /* +0x00: model-space X */
    float    pos_y;                 /* +0x04: model-space Y */
    float    pos_z;                 /* +0x08: model-space Z */
    float    view_x;                /* +0x0C: view-space X (TransformMeshVerticesToView) */
    float    view_y;                /* +0x10: view-space Y (runtime) */
    float    view_z;                /* +0x14: view-space Z (runtime) */
    uint32_t lighting;              /* +0x18: intensity byte in low 8 bits [0x40..0xFF]
                                             (ComputeMeshVertexLighting writes this;
                                              mapped through DAT_004aee68[] LUT for D3DCOLOR) */
    float    tex_u;                 /* +0x1C: primary texture U (clamped, half-texel inset) */
    float    tex_v;                 /* +0x20: primary texture V */
    float    proj_u;                /* +0x24: secondary/projection U (ApplyMeshProjectionEffect) */
    float    proj_v;                /* +0x28: secondary/projection V */
} MeshVertex;

/* ======================================================================
 * Face Normal (16 bytes per vertex normal, 0x10)
 *
 * Stored per-face: 3 consecutive entries for triangles (0x30 total),
 * 4 for quads (0x40 total). The +0x0C field in the FIRST vertex of
 * each face is repurposed as a per-face visibility flag by
 * PrepareMeshResource.
 *
 * ComputeMeshVertexLighting iterates these at stride 0x10 per vertex.
 * ====================================================================== */

typedef struct VertexNormal {
    float    nx;                    /* +0x00: normal X component */
    float    ny;                    /* +0x04: normal Y component
                                             (if < 0.03662 for ANY vertex in face,
                                              face is culled as backfacing) */
    float    nz;                    /* +0x08: normal Z component */
    int32_t  visible_flag;          /* +0x0C: 0=hidden, 1=visible (per-face, in first vertex) */
} VertexNormal;

/* ======================================================================
 * D3D Submitted Vertex (32 bytes, 0x20)
 *
 * After projection, vertices are written to the draw buffer in this
 * format (D3DFVF = 0x1C4: XYZRHW | DIFFUSE | SPECULAR | TEX1).
 * ====================================================================== */

typedef struct D3DSubmittedVertex {
    float    screen_x;              /* +0x00: screen-space X */
    float    screen_y;              /* +0x04: screen-space Y */
    float    rhw;                   /* +0x08: reciprocal homogeneous W (1/Z) */
    float    z_or_pad;              /* +0x0C: Z or padding */
    uint32_t diffuse;               /* +0x10: ARGB diffuse (from lighting LUT) */
    uint32_t specular;              /* +0x14: ARGB specular (fog) */
    float    tu;                    /* +0x18: texture U */
    float    tv;                    /* +0x1C: texture V */
} D3DSubmittedVertex;

_Static_assert(sizeof(MeshResourceHeader) == 0x38, "MeshResourceHeader size drifted from 0x38");
_Static_assert(offsetof(MeshResourceHeader, command_count) == 0x04, "MeshResourceHeader.command_count offset drifted");
_Static_assert(offsetof(MeshResourceHeader, commands_offset) == 0x2C, "MeshResourceHeader.commands_offset offset drifted");
_Static_assert(offsetof(MeshResourceHeader, vertices_offset) == 0x30, "MeshResourceHeader.vertices_offset offset drifted");
_Static_assert(offsetof(MeshResourceHeader, normals_offset) == 0x34, "MeshResourceHeader.normals_offset offset drifted");
_Static_assert(sizeof(PrimitiveCommand) == 0x10, "PrimitiveCommand size drifted from 0x10");
_Static_assert(offsetof(PrimitiveCommand, triangle_count) == 0x08, "PrimitiveCommand.triangle_count offset drifted");
_Static_assert(offsetof(PrimitiveCommand, vertex_data_ptr) == 0x0C, "PrimitiveCommand.vertex_data_ptr offset drifted");
_Static_assert(sizeof(MeshVertex) == 0x2C, "MeshVertex size drifted from 0x2C");
_Static_assert(offsetof(MeshVertex, view_x) == 0x0C, "MeshVertex.view_x offset drifted");
_Static_assert(offsetof(MeshVertex, lighting) == 0x18, "MeshVertex.lighting offset drifted");
_Static_assert(offsetof(MeshVertex, tex_u) == 0x1C, "MeshVertex.tex_u offset drifted");
_Static_assert(sizeof(VertexNormal) == 0x10, "VertexNormal size drifted from 0x10");
_Static_assert(offsetof(VertexNormal, visible_flag) == 0x0C, "VertexNormal.visible_flag offset drifted");
_Static_assert(sizeof(D3DSubmittedVertex) == 0x20, "D3DSubmittedVertex size drifted from 0x20");
_Static_assert(offsetof(D3DSubmittedVertex, diffuse) == 0x10, "D3DSubmittedVertex.diffuse offset drifted");
_Static_assert(offsetof(D3DSubmittedVertex, tu) == 0x18, "D3DSubmittedVertex.tu offset drifted");

/* ======================================================================
 * MODELS.DAT File Format
 *
 * Container for track model groups. Loaded into a 32-byte aligned buffer.
 *
 * Layout:
 *   uint32_t  entry_count
 *   struct { uint32_t block_offset; uint32_t reserved; } entries[entry_count]
 *   // Sub-mesh blocks follow (each block_offset is relative to file start)
 *
 * Each sub-mesh block:
 *   uint32_t  sub_mesh_count
 *   uint32_t  mesh_offsets[sub_mesh_count]  // relative to block start
 *   // Each mesh_offset points to a MeshResourceHeader (PRR format)
 *
 * ParseModelsDat (0x431190) relocates all offsets to absolute pointers
 * and calls PrepareMeshResource on each mesh.
 * ====================================================================== */

/* Runtime globals set by ParseModelsDat */
/* extern uint32_t  gModelsDatEntryCount;     -- number of model groups */
/* extern uint32_t* gModelsDatEntryTable;     -- relocated entry pair array */
/* extern uint32_t* DAT_004aee54;             -- pointer past entry table */

#endif /* TD5_MESH_RESOURCE_H */
