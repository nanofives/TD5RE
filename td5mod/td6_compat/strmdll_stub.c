/*
 * strmdll.dll loader stub for TD6's bundled WMAUDSDK.DLL.
 * strmdll.dll was the NetShow/WMP streaming-format DLL; it is gone on modern
 * Windows. WMAUDSDK imports three functions from it (ASF format-set helpers
 * used on the network-streaming path, not for local-file decode).
 */

#define STRM_E_FAIL ((long)0x80004005L)

long CreateAsfFormatSet(void)  { return STRM_E_FAIL; }
long SelectHelper(void)        { return STRM_E_FAIL; }
long SelectMediaStream(void)   { return STRM_E_FAIL; }
