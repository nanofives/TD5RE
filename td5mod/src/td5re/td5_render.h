/**
 * td5_render.h -- Scene setup, mesh transform, frustum cull
 *
 * Software transform + D3D rasterization pipeline. All vertex transformation,
 * lighting, clipping, and projection are CPU-side. GPU used only for final
 * rasterization via DrawPrimitive (pre-transformed vertices, FVF 0x1C4).
 *
 * Original functions:
 *   0x40AE10  InitializeRaceRenderGlobals
 *   0x40AE80  InitializeRaceRenderState
 *   0x40AEC0  ReleaseRaceRenderResources
 *   0x40ADE0  BeginRaceScene
 *   0x40AE00  EndRaceScene
 *   0x40AF10  ConfigureRaceFogColorAndMode
 *   0x40AF50  ApplyRaceFogRenderState
 *   0x40B070  SetRaceRenderStatePreset (4 presets)
 *   0x43DA80  LoadRenderRotationMatrix
 *   0x43DC20  LoadRenderTranslation
 *   0x43DAF0  PushRenderTransform
 *   0x43DB70  PopRenderTransform
 *   0x43DC50  TransformVec3ByRenderMatrixFull
 *   0x43DD60  TransformMeshVerticesToView
 *   0x43DDF0  ComputeMeshVertexLighting (3-light diffuse)
 *   0x42DCA0  IsBoundingSphereVisibleInCurrentFrustum (5-plane test)
 *   0x42DE10  TestMeshAgainstViewFrustum
 *   0x43DCB0  TransformAndQueueTranslucentMesh
 *   0x4314B0  RenderPreparedMeshResource (dispatch table walk)
 *   0x4317F0  ClipAndSubmitProjectedPolygon (near-plane + screen clip)
 *   0x4323D0  RenderTrackSegmentBatch (X-axis clipper)
 *   0x4326D0  RenderTrackSegmentBatchVariant (Y-axis clipper)
 *   0x432AB0  AppendClippedPolygonTriangleFan
 *   0x431270  RenderTrackSpanDisplayList
 *   0x4312E0  InitializeTranslucentPrimitivePipeline
 *   0x431460  QueueTranslucentPrimitiveBatch
 *   0x431340  FlushQueuedTranslucentPrimitives
 *   0x4329E0  FlushImmediateDrawPrimitiveBatch
 *   0x43E3B0  InsertBillboardIntoDepthSortBuckets
 *   0x43E550  QueueProjectedPrimitiveBucketEntry
 *   0x43E2F0  FlushProjectedPrimitiveBuckets (4096 buckets)
 *   0x43E7E0  ConfigureProjectionForViewport
 *   0x43DEC0  ApplyMeshProjectionEffect (water/envmap UV)
 *   0x40BD20  RenderRaceActorsForView
 *   0x40C7E0  BuildSpecialActorOverlayQuads
 *   0x40B9F0  GetTextureSlotStatus
 *   0x40BA10  AdvanceTexturePageUsageAges
 *   0x40BA60  ResetTexturePageCacheState
 *   0x40BAA0  QueryRaceTextureCapacity
 *   0x40B660  BindRaceTexturePage
 *   0x40B830  AdvanceTextureStreamingScheduler
 *   0x40B1D0  BuildTrackTextureCacheImpl
 *   0x430D30  ParseAndDecodeCompressedTrackData
 *   0x43D7C0  AdvanceGlobalSkyRotation
 *   0x43CDC0  AdvanceWorldBillboardAnimations
 *   0x446F00  RenderVehicleWheelBillboards
 *   0x43F210  RenderTireTrackPool
 */

#ifndef TD5_RENDER_H
#define TD5_RENDER_H

#include "td5_types.h"

/* --- Module lifecycle --- */
int  td5_render_init(void);
void td5_render_shutdown(void);
void td5_render_frame(void);

/* --- Scene brackets --- */
void td5_render_begin_scene(void);
void td5_render_end_scene(void);

/* --- Render state --- */
void td5_render_set_preset(TD5_RenderPreset preset);
void td5_render_set_fog(int enable);
void td5_render_configure_fog(uint32_t color, int enable);

/* --- Transform stack (single-depth) --- */
void td5_render_load_rotation(const TD5_Mat3x3 *rot);
void td5_render_load_translation(const TD5_Vec3f *pos);
void td5_render_push_transform(void);
void td5_render_pop_transform(void);
void td5_render_transform_vec3(const float *in, float *out);

/* --- Vertex transform & lighting --- */
void td5_render_transform_mesh_vertices(TD5_MeshHeader *mesh);
void td5_render_compute_vertex_lighting(TD5_MeshHeader *mesh);

/* --- Frustum culling --- */
int  td5_render_is_sphere_visible(float cx, float cy, float cz, float radius);
int  td5_render_test_mesh_frustum(TD5_MeshHeader *mesh, float *out_depth);

/* --- Mesh rendering --- */
void td5_render_span_display_list(void *display_list_block);
void td5_render_prepared_mesh(TD5_MeshHeader *mesh);
void td5_render_actors_for_view(int view_index);
void td5_render_set_vehicle_mesh(int slot, TD5_MeshHeader *mesh);
TD5_MeshHeader *td5_render_get_vehicle_mesh(int slot);

/* --- Projection --- */
void td5_render_configure_projection(int width, int height);

/* --- Translucent pipeline --- */
void td5_render_init_translucent_pipeline(void);
void td5_render_queue_translucent_batch(void *record);
void td5_render_flush_translucent(void);
void td5_render_flush_immediate_batch(void);

/* --- Depth-sorted pipeline (4096 buckets) --- */
void td5_render_queue_projected_entry(void *entry, int bucket, uint32_t flags, int texture_page);
void td5_render_flush_projected_buckets(void);

/* --- Texture cache --- */
void td5_render_reset_texture_cache(void);
void td5_render_advance_texture_ages(void);
int  td5_render_bind_texture_page(int page_id);

/* --- Sky --- */
void td5_render_advance_sky_rotation(void);
void td5_render_load_sky(const char *path);
void td5_render_draw_sky(void);

/* --- Billboard animation --- */
void td5_render_advance_billboard_anims(void);

#endif /* TD5_RENDER_H */
