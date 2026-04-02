/**
 * td5_png_replace.h - PNG-to-TGA replacement module
 *
 * Intercepts TGA file loads from ZIP archives via ReadArchiveEntry and
 * replaces them with PNG textures decoded to TGA format in memory.
 * Works independently of the rendering backend.
 *
 * INI config:
 *   [PngReplace]
 *   Enable=1
 *   PngDir=td5_png_clean
 */

#ifndef TD5_PNG_REPLACE_H
#define TD5_PNG_REPLACE_H

#include <windows.h>
#include <stdint.h>

/**
 * Initialize PNG replacement from INI config.
 * Returns 1 if enabled, 0 if disabled.
 */
int PngReplace_Init(const char *ini_path);

/**
 * Try to replace a TGA archive entry with PNG data decoded to TGA format.
 * Returns bytes written to destBuf (TGA format), or 0 if no replacement.
 */
uint32_t PngReplace_TryReplace(const char *entryName, const char *zipPath,
                                char *destBuf, uint32_t maxSize);

/**
 * Return the TGA size that would result from PNG replacement, or 0 if N/A.
 * Used by Hook_GetArchiveEntrySize.
 */
uint32_t PngReplace_GetTgaSize(const char *entryName, const char *zipPath);

/**
 * Shutdown. Call at DLL_PROCESS_DETACH.
 */
void PngReplace_Shutdown(void);

#endif /* TD5_PNG_REPLACE_H */
