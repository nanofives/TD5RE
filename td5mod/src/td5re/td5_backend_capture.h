/**
 * td5_backend_capture.h -- port-side declarations for the D3D11 backend's
 * framebuffer read-back API (implemented in ddraw_wrapper/d3d11_backend_device.c).
 *
 * A capture is requested for the next presented frame with
 * Backend_RequestCapture(); the composited backbuffer is then readable one
 * frame later via Backend_GetCapture() as BGRA pixels. Backend_CaptureDebug()
 * exposes the capture-path counters (serviced/failed/ok + the last failing
 * CreateTexture2D HRESULT and backbuffer desc) so a caller can tell a genuine
 * read-back failure (e.g. DXGI_ERROR_DEVICE_REMOVED) apart from a real result.
 *
 * Consumers: the offline photo-booth (td5_game.c) and the self-test
 * render-golden layer (td5_selftest.c). Declaring the API here keeps these
 * cross-module calls out of per-.c `extern` decls (structure ratchet).
 */
#ifndef TD5_BACKEND_CAPTURE_H
#define TD5_BACKEND_CAPTURE_H

/** Request a read-back of the next presented frame. */
void Backend_RequestCapture(void);

/** Retrieve the most recent capture. Returns 1 and fills px (BGRA, w*h*4) / w /
 *  h when a frame is ready, else 0. The buffer is owned by the backend. */
int  Backend_GetCapture(unsigned char **px, int *w, int *h);

/** Fill a caller array of >= 13 unsigned with capture-path counters:
 *  [0]=serviced [1]=no_swapchain [2]=getbuffer_fail [3]=createtexture_fail
 *  [4]=map_fail [5]=ok [6]=last_createtexture_hr [7]=backbuffer_format
 *  [8]=sample_count [9]=width [10]=height [11]=bind_flags [12]=misc_flags. */
void Backend_CaptureDebug(unsigned *out);

#endif /* TD5_BACKEND_CAPTURE_H */
