#ifndef TD5_DDRAW_H
#define TD5_DDRAW_H

#include <windows.h>
#include <ddraw.h>

typedef HRESULT (WINAPI *PFN_DDCreate)(GUID*, LPDIRECTDRAW*, IUnknown*);

extern HMODULE      g_real_ddraw;
extern PFN_DDCreate g_real_DDCreate;

void td5dd_log(const char *fmt, ...);

/* idd_wrap.c */
LPDIRECTDRAW   td5dd_wrap_dd1(LPDIRECTDRAW  real);
LPDIRECTDRAW4  td5dd_wrap_dd4(LPDIRECTDRAW4 real);
LPDIRECTDRAW7  td5dd_wrap_dd7(LPDIRECTDRAW7 real);

/* HWND the game registered via SetCooperativeLevel. Used by the idd wrapper
 * to force the window's client area to match the requested render size. */
extern HWND g_td5dd_hwnd;

#endif
