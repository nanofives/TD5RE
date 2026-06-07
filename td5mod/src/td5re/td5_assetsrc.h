/**
 * td5_assetsrc.h -- Editable-source "pack-on-load" asset layer.
 *
 * Retires binary .DAT assets: when an editable source file (JSON / CSV /
 * PNG+sidecar / glTF) exists where a .DAT would live, the runtime encodes it to
 * the exact binary layout the existing parsers expect, in memory, at load time.
 * The loose .DAT files are then deleted from re/assets/.
 *
 * Wired as "step 0" of td5_asset_open_and_read() (td5_asset.c): if
 * td5_assetsrc_pack() returns a buffer it is used verbatim; otherwise the loader
 * falls through to the legacy loose-file / extracted-folder / ZIP path.
 *
 * Phase 0: the registry is empty -> td5_assetsrc_pack() always returns NULL, so
 * the runtime behaves identically. Per-format encoders are added in later
 * phases (Tier 1 tables, STRIP, TEXTURES, glTF meshes).
 */
#ifndef TD5_ASSETSRC_H
#define TD5_ASSETSRC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * If an editable source is registered AND present on disk for `entry_name`
 * (e.g. "STRIP.DAT"), encode it to the original binary layout and return a
 * malloc'd buffer (caller owns it -- free(), same ownership contract as
 * td5_asset_open_and_read). *out_size receives the packed size.
 *
 * Returns NULL when no editable source applies, so the caller falls through to
 * the legacy .DAT loading paths.
 */
void *td5_assetsrc_pack(const char *entry_name, const char *zip_path,
                        int *out_size);

/**
 * Packed size of the editable source for `entry_name`, or -1 if none applies.
 * Mirrors td5_assetsrc_pack for the size-only query path.
 */
int td5_assetsrc_packed_size(const char *entry_name, const char *zip_path);

/**
 * Byte-exact self-test. For every registered byte-exact format that has both an
 * editable source and an original .DAT present, pack the source and compare it
 * against the .DAT, printing PASS/FAIL per file. Returns 0 if all pass (or none
 * are found), nonzero on any mismatch. (`root` reserved for a future scoped
 * sweep; pass NULL for the default asset tree.)
 */
int td5_assetsrc_selftest(const char *root);

#ifdef __cplusplus
}
#endif

#endif /* TD5_ASSETSRC_H */
