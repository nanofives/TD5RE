/*
 * td5_gdi_text.dll  --  GDI Text Rendering Replacement for Test Drive 5
 *
 * Replaces the bitmap glyph atlas text rendering with Windows GDI TextOut
 * for crisp, scalable text.  Loaded at runtime via a code cave injected
 * into InitializeFrontendResourcesAndState by patch_gdi_text.py.
 *
 * Phase 1: Frontend small font  (body text, buttons, options)
 * Phase 2: Frontend large font  (menu titles)
 *
 * Build (MSVC x86, from VS x86 Native Tools Command Prompt):
 *   cl /LD /O2 td5_gdi_text.c /link user32.lib gdi32.lib kernel32.lib /OUT:td5_gdi_text.dll
 *
 * Build (MinGW 32-bit):
 *   i686-w64-mingw32-gcc -shared -O2 td5_gdi_text.c -o td5_gdi_text.dll -lgdi32 -luser32
 *
 * Place the built td5_gdi_text.dll next to TD5_d3d.exe.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

/* ===================================================================
 * DEBUG LOG -- writes to td5_gdi_text.log next to the DLL
 * =================================================================== */

static FILE *g_logFile = NULL;

static void LogOpen(void)
{
    g_logFile = fopen("td5_gdi_text.log", "w");
}

static void Log(const char *fmt, ...)
{
    va_list ap;
    if (!g_logFile) return;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fflush(g_logFile);
}

/* ===================================================================
 * CONFIGURATION -- adjust these to change font appearance and size
 * =================================================================== */

/* Font heights in pixels (negative = precise character height) */
#define SMALL_FONT_HEIGHT   -14     /* Body text, options, buttons          */
#define LARGE_FONT_HEIGHT   -28     /* Menu titles                          */

#define FONT_WEIGHT         FW_BOLD
#define FONT_FACE           "Arial"

/* Text colour (white to match original) */
#define TEXT_COLOR           RGB(255, 255, 255)

/* ===================================================================
 * EXE ADDRESSES -- known globals in TD5_d3d.exe (base 0x00400000)
 * =================================================================== */

/* DirectDraw surface pointers (IDirectDrawSurface*) */
#define P_PRIMARY_SURF      ((void **)0x00496260)   /* primary 640x480 work     */
#define P_SECONDARY_SURF    ((void **)0x00496264)   /* secondary work surface   */
#define P_MENUFONT_SURF     ((void **)0x00496278)   /* MenuFont.tga surface     */
#define P_SMALLFONT_SURF    ((void **)0x0049627C)   /* active small font surf   */
#define P_ARROW_EXTRAS      ((void **)0x00496288)   /* ArrowExtras.tga surface  */

/* Per-character width tables (signed char[128] in .data section) */
#define TBL_SMALL_ADVANCE   ((signed char *)0x004662D0) /* small advance widths */
#define TBL_SMALL_DISPLAY   ((signed char *)0x004662F0) /* small display widths */
#define TBL_SMALL_YOFFSET   ((signed char *)0x004663E4) /* small Y-offsets      */
#define TBL_LARGE_ADVANCE   ((signed char *)0x004664F8) /* large advance widths */
#define TBL_LARGE_DISPLAY   ((signed char *)0x00466518) /* large display widths */

/* Original function addresses (hook targets)
 * These are the ACTUALLY CALLED functions (verified via E8 call search).
 * The inner functions (0x424180 etc.) have zero callers. */
#define VA_LocDrawPrimary   0x004242B0  /* DrawFrontendLocalizedStringPrimary   (4 callers)  */
#define VA_LocDrawSecondary 0x00424390  /* DrawFrontendLocalizedStringSecondary (1 caller)   */
#define VA_LocDrawToSurf    0x00424560  /* DrawFrontendLocalizedStringToSurface (5 callers)  */
#define VA_MeasureOrDraw    0x00412D50  /* MeasureOrDrawFrontendFontString      (2 callers)  */
#define VA_MeasureOrCenter  0x00424A50  /* MeasureOrCenterFrontendLocalizedStr  (10 callers) */

/* IDirectDrawSurface COM vtable indices */
#define VT_BLTFAST          7           /* offset 0x1C */
#define VT_GETDC            17          /* offset 0x44 */
#define VT_RELEASEDC        26          /* offset 0x68 */

/* ===================================================================
 * COM CALL TYPEDEFS
 * =================================================================== */

typedef HRESULT (__stdcall *FN_GetDC)(void *self, HDC *phdc);
typedef HRESULT (__stdcall *FN_ReleaseDC)(void *self, HDC hdc);
typedef HRESULT (__stdcall *FN_BltFast)(void *self, DWORD dwX, DWORD dwY,
                                        void *lpSrc, RECT *lpSrcRect, DWORD dwFlags);

/* ===================================================================
 * GLOBALS
 * =================================================================== */

static HFONT     g_hFontSmall  = NULL;
static HFONT     g_hFontLarge  = NULL;
static COLORREF  g_textColor   = TEXT_COLOR;
static HDC       g_hMeasureDC  = NULL;  /* persistent DC for fast measurement */
static int       g_drawCallCount = 0;   /* for debug logging */

/* ===================================================================
 * DIRECTDRAW SURFACE HELPERS
 * =================================================================== */

static HDC SurfGetDC(void *surf)
{
    HDC hdc = NULL;
    void **vt;
    FN_GetDC fn;
    if (!surf) return NULL;
    vt = *(void ***)surf;
    fn = (FN_GetDC)vt[VT_GETDC];
    if (SUCCEEDED(fn(surf, &hdc)))
        return hdc;
    return NULL;
}

static void SurfReleaseDC(void *surf, HDC hdc)
{
    void **vt = *(void ***)surf;
    FN_ReleaseDC fn = (FN_ReleaseDC)vt[VT_RELEASEDC];
    fn(surf, hdc);
}

static void SurfBltFast(void *dest, int x, int y, void *src,
                        RECT *srcRect, DWORD flags)
{
    void **vt = *(void ***)dest;
    FN_BltFast fn = (FN_BltFast)vt[VT_BLTFAST];
    fn(dest, (DWORD)x, (DWORD)y, src, srcRect, flags);
}

/* ===================================================================
 * GDI TEXT HELPERS
 * =================================================================== */

/* Draw a string to a DDraw surface.  Returns the rendered pixel width. */
static int GdiDrawToSurface(void *surf, const char *str, int x, int y,
                            HFONT font)
{
    HDC   hdc;
    HFONT old;
    int   len;
    SIZE  sz;

    if (!surf || !str || !*str) return 0;

    hdc = SurfGetDC(surf);
    if (!hdc) return 0;

    old = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_textColor);

    len = lstrlenA(str);
    TextOutA(hdc, x, y, str, len);
    GetTextExtentPoint32A(hdc, str, len, &sz);

    SelectObject(hdc, old);
    SurfReleaseDC(surf, hdc);

    return sz.cx;
}

/* Measure a string width without drawing.  Uses persistent measurement DC. */
static int GdiMeasure(const char *str, HFONT font)
{
    HFONT old;
    int   len;
    SIZE  sz;

    if (!str || !*str) return 0;

    if (!g_hMeasureDC) {
        g_hMeasureDC = CreateCompatibleDC(NULL);
        if (!g_hMeasureDC) return 0;
    }

    old = (HFONT)SelectObject(g_hMeasureDC, font);
    len = lstrlenA(str);
    GetTextExtentPoint32A(g_hMeasureDC, str, len, &sz);
    SelectObject(g_hMeasureDC, old);

    return sz.cx;
}

/* Measure a single character width. */
static int GdiMeasureChar(char ch, HFONT font)
{
    HFONT old;
    SIZE  sz;

    if (!g_hMeasureDC) {
        g_hMeasureDC = CreateCompatibleDC(NULL);
        if (!g_hMeasureDC) return 8;
    }

    old = (HFONT)SelectObject(g_hMeasureDC, font);
    GetTextExtentPoint32A(g_hMeasureDC, &ch, 1, &sz);
    SelectObject(g_hMeasureDC, old);
    return sz.cx;
}

/* ===================================================================
 * REPLACEMENT FUNCTIONS  (match original __cdecl signatures exactly)
 * =================================================================== */

/*
 * DrawFrontendLocalizedStringPrimary (0x4242B0)
 * Draws body text onto the primary 640x480 work surface.
 * Original: dispatches English (inline 24x24) vs non-English (12px grid).
 * Our version: always uses GDI regardless of language.
 */
__declspec(dllexport) int __cdecl
GDI_DrawLocalizedStringPrimary(const char *str, unsigned int x, int y)
{
    int w;
    if (!str || !*str) return 0;
    Log("DrawLocPrimary: \"%s\" x=%u y=%d surf=0x%08X\n",
        str, x, y, (DWORD)(ULONG_PTR)*P_PRIMARY_SURF);
    w = GdiDrawToSurface(*P_PRIMARY_SURF, str, (int)x, y, g_hFontSmall);
    Log("  -> width=%d\n", w);
    return w;
}

/*
 * DrawFrontendLocalizedStringSecondary (0x424390)
 * Same as Primary but targets the secondary work surface.
 */
__declspec(dllexport) int __cdecl
GDI_DrawLocalizedStringSecondary(const char *str, unsigned int x, int y)
{
    if (!str || !*str) return 0;
    Log("DrawLocSecondary: \"%s\" x=%u y=%d\n", str, x, y);
    return GdiDrawToSurface(*P_SECONDARY_SURF, str, (int)x, y, g_hFontSmall);
}

/*
 * DrawFrontendLocalizedStringToSurface (0x424560)
 * Draws text onto an arbitrary DDraw surface (buttons, options, etc.).
 */
__declspec(dllexport) int __cdecl
GDI_DrawLocalizedStringToSurface(const char *str, int x, int y, void *destSurf)
{
    int w;
    if (!str || !*str) return 0;
    Log("DrawLocToSurf: \"%s\" x=%d y=%d surf=0x%08X\n",
        str, x, y, (DWORD)(ULONG_PTR)destSurf);
    w = GdiDrawToSurface(destSurf, str, x, y, g_hFontSmall);
    Log("  -> width=%d\n", w);
    return w;
}

/*
 * MeasureOrDrawFrontendFontString (0x412D50)
 * If destSurf == NULL: measure only (returns pixel width).
 * Otherwise: draw menu title text using the large font.
 */
__declspec(dllexport) int __cdecl
GDI_MeasureOrDrawFrontendFontString(const char *str, unsigned int x,
                                    unsigned int y, void *destSurf)
{
    int w;
    if (!str || !*str) return 0;

    if (!destSurf) {
        w = GdiMeasure(str, g_hFontLarge) + 4;
        Log("MeasureOrDraw MEASURE: \"%s\" -> %d\n", str, w);
        return w;
    }

    w = GdiDrawToSurface(destSurf, str, (int)x, (int)y, g_hFontLarge);
    Log("MeasureOrDraw DRAW: \"%s\" x=%u y=%u -> %d\n", str, x, y, w);
    return w;
}

/*
 * MeasureOrCenterFrontendLocalizedString (0x424A50)
 * If totalWidth == 0: returns pixel width of str (measure mode).
 * If totalWidth != 0: returns centered X position.
 */
__declspec(dllexport) int __cdecl
GDI_MeasureOrCenterLocalizedString(const char *str, int startX, int totalWidth)
{
    int textW;
    if (!str || !*str) return startX;

    textW = GdiMeasure(str, g_hFontSmall);
    Log("MeasureOrCenter: \"%s\" startX=%d totalW=%d textW=%d\n",
        str, startX, totalWidth, textW);

    if (totalWidth == 0)
        return textW;

    /* Center: startX + (totalWidth - textW - startX) / 2 */
    return startX + (unsigned int)((totalWidth - textW - startX)) / 2;
}

/* ===================================================================
 * HOOK INSTALLATION  (patches JMP at each function entry)
 * =================================================================== */

static void InstallJmpHook(DWORD targetVA, void *newFunc)
{
    DWORD oldProt;
    unsigned char *target = (unsigned char *)(ULONG_PTR)targetVA;

    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
        Log("  FAILED VirtualProtect at 0x%08X (err=%lu)\n", targetVA, GetLastError());
        return;
    }

    target[0] = 0xE9;  /* JMP rel32 */
    *(DWORD *)(target + 1) = (DWORD)(ULONG_PTR)newFunc - (targetVA + 5);

    VirtualProtect(target, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    Log("  OK hook 0x%08X -> 0x%08X\n", targetVA, (DWORD)(ULONG_PTR)newFunc);
}

/* ===================================================================
 * WIDTH TABLE UPDATE  (populate with GDI font metrics)
 * =================================================================== */

static void UpdateWidthTables(void)
{
    HDC   hdc;
    HFONT old;
    DWORD prot;
    int   c;
    SIZE  sz;
    char  ch;

    hdc = CreateCompatibleDC(NULL);
    if (!hdc) return;

    /* --- Small font tables --- */
    old = (HFONT)SelectObject(hdc, g_hFontSmall);

    VirtualProtect(TBL_SMALL_ADVANCE, 128, PAGE_READWRITE, &prot);
    VirtualProtect(TBL_SMALL_DISPLAY, 128, PAGE_READWRITE, &prot);
    VirtualProtect(TBL_SMALL_YOFFSET, 128, PAGE_READWRITE, &prot);

    for (c = 0; c < 128; c++) {
        if (c < 0x20) {
            TBL_SMALL_ADVANCE[c] = 0;
            TBL_SMALL_DISPLAY[c] = 0;
            TBL_SMALL_YOFFSET[c] = 0;
            continue;
        }
        ch = (char)c;
        GetTextExtentPoint32A(hdc, &ch, 1, &sz);
        TBL_SMALL_ADVANCE[c] = (signed char)(sz.cx > 120 ? 120 : sz.cx);
        TBL_SMALL_DISPLAY[c] = TBL_SMALL_ADVANCE[c];
        TBL_SMALL_YOFFSET[c] = 0;  /* GDI handles baseline internally */
    }

    /* --- Large font tables --- */
    SelectObject(hdc, g_hFontLarge);

    VirtualProtect(TBL_LARGE_ADVANCE, 128, PAGE_READWRITE, &prot);
    VirtualProtect(TBL_LARGE_DISPLAY, 128, PAGE_READWRITE, &prot);

    for (c = 0; c < 128; c++) {
        if (c < 0x20) {
            TBL_LARGE_ADVANCE[c] = 0;
            TBL_LARGE_DISPLAY[c] = 0;
            continue;
        }
        ch = (char)c;
        GetTextExtentPoint32A(hdc, &ch, 1, &sz);
        TBL_LARGE_ADVANCE[c] = (signed char)(sz.cx > 120 ? 120 : sz.cx);
        TBL_LARGE_DISPLAY[c] = TBL_LARGE_ADVANCE[c];
    }

    SelectObject(hdc, old);
    DeleteDC(hdc);
}

/* ===================================================================
 * EXPORTED INIT  (called from the EXE code cave after font surfaces load)
 * =================================================================== */

__declspec(dllexport) void __cdecl TD5_InitGdiFonts(void)
{
    LogOpen();
    Log("=== TD5_InitGdiFonts called ===\n");

    /* Check surface globals */
    Log("Surface ptrs: primary=0x%08X secondary=0x%08X menufont=0x%08X\n",
        (DWORD)(ULONG_PTR)*P_PRIMARY_SURF,
        (DWORD)(ULONG_PTR)*P_SECONDARY_SURF,
        (DWORD)(ULONG_PTR)*P_MENUFONT_SURF);

    /* Create the two GDI fonts */
    g_hFontSmall = CreateFontA(
        SMALL_FONT_HEIGHT, 0, 0, 0,
        FONT_WEIGHT, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FF_SWISS | VARIABLE_PITCH,
        FONT_FACE);

    g_hFontLarge = CreateFontA(
        LARGE_FONT_HEIGHT, 0, 0, 0,
        FONT_WEIGHT, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FF_SWISS | VARIABLE_PITCH,
        FONT_FACE);

    Log("Fonts: small=0x%08X large=0x%08X\n",
        (DWORD)(ULONG_PTR)g_hFontSmall, (DWORD)(ULONG_PTR)g_hFontLarge);

    if (!g_hFontSmall || !g_hFontLarge) {
        Log("ABORT: CreateFont failed!\n");
        return;
    }

    /* Test GetDC on primary surface */
    {
        void *surf = *P_PRIMARY_SURF;
        if (surf) {
            HDC testDC = SurfGetDC(surf);
            Log("Test GetDC on primary surface: HDC=0x%08X\n",
                (DWORD)(ULONG_PTR)testDC);
            if (testDC) SurfReleaseDC(surf, testDC);
        } else {
            Log("WARNING: primary surface is NULL!\n");
        }
    }

    /* Populate width tables with GDI-measured metrics */
    UpdateWidthTables();
    Log("Width tables updated. Sample: 'A' small=%d large=%d\n",
        (int)TBL_SMALL_ADVANCE['A'], (int)TBL_LARGE_ADVANCE['A']);

    /* Install JMP hooks at each actually-called function entry */
    Log("Installing hooks:\n");
    InstallJmpHook(VA_LocDrawPrimary,   GDI_DrawLocalizedStringPrimary);
    InstallJmpHook(VA_LocDrawSecondary, GDI_DrawLocalizedStringSecondary);
    InstallJmpHook(VA_LocDrawToSurf,    GDI_DrawLocalizedStringToSurface);
    InstallJmpHook(VA_MeasureOrDraw,    GDI_MeasureOrDrawFrontendFontString);
    InstallJmpHook(VA_MeasureOrCenter,  GDI_MeasureOrCenterLocalizedString);

    Log("=== Init complete ===\n");

    /* DEBUG: Log when re-entering frontend (second call to init) */
    {
        static int s_initCount = 0;
        s_initCount++;
        Log("Init call #%d\n", s_initCount);
    }
}

/* ===================================================================
 * DLL ENTRY POINT
 * =================================================================== */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL; (void)fdwReason; (void)lpvReserved;
    return TRUE;
}
