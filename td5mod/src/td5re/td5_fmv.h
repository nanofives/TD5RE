/**
 * td5_fmv.h -- FMV playback module (replaces EA TGQ codec)
 *
 * The original game uses EA TGQ format for Movie/intro.tgq, decoded via
 * a custom JPEG-IDCT codec with YCbCr 4:2:0 color, EA ADPCM audio, and
 * DirectDraw surface presentation (see ea-tgq-multimedia-engine.md).
 *
 * The source port replaces this entirely with a modern video player:
 *   - Primary backend: Windows Media Foundation (IMFSourceReader)
 *   - Supports: MP4, AVI, WMV, and any format with installed MF codecs
 *   - TGQ files should be transcoded to MP4 at install/build time
 *
 * Legal screens (originally from LEGALS.ZIP as TGA) are displayed using
 * the D3D11 rendering backend via td5_platform.h.
 *
 * Original functions replaced:
 *   0x43C440  PlayIntroMovie      -> td5_fmv_play()
 *   0x42C8E0  ShowLegalScreens    -> td5_fmv_show_legal_screens()
 *             OpenAndStartMediaPlayback (M2DX.dll) -> internal
 */

#ifndef TD5_FMV_H
#define TD5_FMV_H

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

/**
 * Initialize the FMV subsystem (COM, Media Foundation).
 * Call once at startup after td5_plat_window_create().
 * Returns 1 on success, 0 on failure.
 */
int td5_fmv_init(void);

/**
 * Shutdown the FMV subsystem and release all resources.
 */
void td5_fmv_shutdown(void);

/* ========================================================================
 * Video Playback
 * ======================================================================== */

/**
 * Play a video file. Blocks until complete or skipped.
 *
 * Supported formats depend on installed Media Foundation codecs.
 * Typical: MP4 (H.264/AAC), AVI, WMV. TGQ is NOT supported --
 * transcode to MP4 first.
 *
 * Skip keys (matching original): Enter, Shift, Escape, Space.
 * Volume adjustable with +/- during playback (matching original).
 *
 * @param filename  Path to the video file (relative or absolute).
 * @return 1 if played to completion, 0 if skipped or error.
 */
int td5_fmv_play(const char *filename);

/**
 * Signal the currently playing video to stop.
 * Safe to call from any thread (uses atomic flag).
 * No-op if nothing is playing.
 */
void td5_fmv_skip(void);

/**
 * Check if FMV playback is available on this platform.
 * Returns 0 if Media Foundation initialization failed or
 * required components are missing.
 */
int td5_fmv_is_supported(void);

/* ========================================================================
 * Legal Screens
 * ======================================================================== */

/**
 * Display legal/copyright screens at startup.
 *
 * Original behavior (ShowLegalScreens @ 0x42C8E0):
 *   - Loads legal1.tga and legal2.tga from LEGALS.ZIP
 *   - Displays each for ~5 seconds with fade
 *   - Skippable via Enter/Escape/Space
 *
 * Source port loads TGA files from Legal/ directory (pre-extracted).
 * Falls back gracefully if files are missing.
 */
void td5_fmv_show_legal_screens(void);

#endif /* TD5_FMV_H */
