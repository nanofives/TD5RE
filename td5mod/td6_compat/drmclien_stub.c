/*
 * DRMClien.DLL loader stub for TD6's bundled WMAUDSDK.DLL (1999 WMA SDK).
 * The real DRMClien.DLL (Windows Media Rights Manager client) no longer
 * exists on modern Windows, so TD6.exe dies at load with STATUS_DLL_NOT_FOUND
 * before reaching WinMain. The game's Music/*.wma are not DRM-protected, so
 * these entry points are only needed to satisfy the import table; they are
 * not expected to be called during plain WMA decode.
 *
 * Import surface (from objdump of WMAUDSDK.DLL):
 *   by name:    ??0CDRMLiteCrypto@@QAE@XZ        (ctor,  thiscall, 0 args)
 *               ??1CDRMLiteCrypto@@QAE@XZ        (dtor,  thiscall, 0 args)
 *               ?GetPublicKey@CDRMLiteCrypto@@QAEJPAUPKCERT@@@Z (thiscall, 1 arg)
 *   by ordinal: 3, 4, 5, 6, 7, 9                 (signatures unknown)
 */

#define DRM_E_FAIL ((long)0x80004005L)

__attribute__((thiscall)) void *drm_lite_ctor(void *thisptr)
{
    return thisptr;
}

__attribute__((thiscall)) void drm_lite_dtor(void *thisptr)
{
    (void)thisptr;
}

__attribute__((thiscall)) long drm_lite_get_public_key(void *thisptr, void *pkcert)
{
    (void)thisptr;
    (void)pkcert;
    return DRM_E_FAIL;
}

/* Unknown-signature ordinal exports. If one of these is ever actually
 * invoked the stack may be left imbalanced (unknown stdcall arity) — that
 * means DRM-protected content was hit and the stub approach is wrong. */
int drm_ordinal_stub(void)
{
    return DRM_E_FAIL;
}
