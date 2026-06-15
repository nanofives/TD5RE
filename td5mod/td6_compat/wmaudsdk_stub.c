/*
 * FALLBACK ONLY — full replacement stub for WMAUDSDK.DLL.
 * Deploy this INSTEAD of the real WMAUDSDK.DLL only if the
 * DRMClien/strmdll loader stubs prove insufficient (i.e. the real decoder
 * calls into a dead DRM path at init). Cost: no WMA music.
 *
 * TD6.exe imports exactly one function, by ordinal 1: WMAudioCreateReader.
 * Callsite FUN_0040e6c0 @0x0040e7e5 checks the HRESULT and logs
 * "Failed to start playing %s" on failure, then continues — so returning
 * E_FAIL degrades gracefully to a silent music channel.
 */

__attribute__((stdcall)) long WMAudioCreateReader(const unsigned short *wpath,
                                                  void *callback,
                                                  void **reader_out,
                                                  unsigned long flags)
{
    (void)wpath;
    (void)callback;
    (void)flags;
    if (reader_out)
        *reader_out = 0;
    return (long)0x80004005L; /* E_FAIL */
}
