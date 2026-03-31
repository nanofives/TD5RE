# M2DXFX.dll -- Last Three Unnamed Functions

Analysis of the final 3 unnamed functions needed for 100% naming coverage.

---

## 1. FUN_100029d0 -- Proposed name: `DXDraw::DisplayFrameRate`

**Subsystem:** DXDraw (frame rate overlay rendering)

### Callers
- `CalculateFrameRate` (0x10001670) -- computes frame rate, then calls this to display it
- `FUN_100029a0` (0x100029a0) -- a thin wrapper that stores fps/tps/mpps globals then calls FUN_100029d0

### Callees
- `wsprintfA`, `SelectObject`, `SetTextColor`, `SetBkColor`, `SetBkMode`
- `GetTextExtentPoint32A`, `SetRect`, `ExtTextOutA`
- `DXErrorToString`, `Msg`, `__ftol`
- IDirectDrawSurface vtable: offset 0x44 = `GetDC`, offset 0x68 = `ReleaseDC`

### Decompiled Code

```c
undefined4 DXDraw_DisplayFrameRate(void)
{
  UINT c;
  ulonglong uVar1;
  HDC hdc;
  HDC local_d0[5];
  CHAR local_bc[32];    // mpps string
  CHAR local_9c[32];    // fps string
  CHAR local_7c[24];    // tps string
  CHAR aCStack_64[8];
  CHAR local_5c[92];    // combined output string

  if (DAT_100214fc != (int *)0x0) {              // DDSurface pointer check
    if (_DAT_10022c0c <= _DAT_1001724c) {
      local_9c[0] = '\0';                        // no fps to display
    }
    else {
      uVar1 = __ftol();
      wsprintfA(local_9c, " %d.%02d fps",
                (int)uVar1 / 100,
                (int)((longlong)(...) % 100));
    }
    if (DAT_10022c10 < 1) {
      local_7c[0] = '\0';
    }
    else {
      wsprintfA(local_7c, " %ld tps", DAT_10022c10);
    }
    if (DAT_10022c14 < 1) {
      local_bc[0] = '\0';
    }
    else {
      wsprintfA(local_bc, " %ld mpps", DAT_10022c14);
    }
    c = wsprintfA(local_5c, " %s %s %s", local_9c, local_7c, local_bc);
    if (DAT_100214fc == (int *)0x0) {
      return 0;
    }
    hdc = local_d0;
    DX::LastError = (**(code **)(*DAT_100214fc + 0x44))(DAT_100214fc);   // GetDC
    if (DX::LastError != 0) {
      DXErrorToString(DX::LastError);
      Msg("GetDC for frame rate buffer fail");
      return 0;
    }
    SelectObject(hdc, DAT_10051fec);              // select font
    SetTextColor(hdc, 0xffff);                    // yellow text
    SetBkColor(hdc, 0);                           // black background
    SetBkMode(hdc, 2);                            // OPAQUE
    GetTextExtentPoint32A(hdc, aCStack_64, c, (LPSIZE)&DAT_1002150c);
    SetRect((LPRECT)&stack0xffffff2c, 0, 0, DAT_1002150c, DAT_10021510);
    ExtTextOutA(hdc, 0, 0, 2, (RECT *)&stack0xffffff2c, aCStack_64, c, NULL);
    (**(code **)(*DAT_100214fc + 0x68))(DAT_100214fc, hdc);              // ReleaseDC
  }
  return 1;
}
```

### Purpose
Renders the frame rate / ticks-per-second / million-pixels-per-second overlay text onto the DirectDraw back buffer using GDI. Formats up to three metric strings (fps, tps, mpps), acquires a DC from the surface, draws yellow-on-black text at the top-left corner, and releases the DC. This is the visual output half of the frame rate diagnostic system; `CalculateFrameRate` computes the values and then calls this function to draw them.

### M2DX.dll Equivalent
Address 0x100029d0 in M2DX.dll falls inside `FUN_10002920`, which is a completely different function (viewport/transform setup with CreateViewport, SetViewport2, SetTransform calls). The DLLs have different code layouts, so addresses do not correspond 1:1. M2DX.dll likely has an equivalent `DisplayFrameRate` at a different address.

---

## 2. FUN_10003de0 -- Proposed name: `DXDraw::FPSDialogProc`

**Subsystem:** DXDraw (FPS name/caption input dialog)

### Callers
- No direct CALL references; passed as a DLGPROC callback to `DialogBoxParamA` in `FUN_10003e80` (0x10003e80)

### Data reference
- Referenced at 0x10003e98 inside FUN_10003e80

### Callees
- `GetDlgItemTextA`, `SetDlgItemTextA`, `GetDlgItem`, `SetFocus`, `EndDialog`

### Decompiled Code

```c
undefined4 DXDraw_FPSDialogProc(HWND param_1, int param_2, short param_3)
{
  HWND hWnd;

  if (param_2 != 0x110) {                        // not WM_INITDIALOG
    if (((param_2 == 0x111) &&                    // WM_COMMAND
         (param_3 == 0x3fe)) &&                   // control ID 1022 (OK button)
        (DAT_1001c04c == 0)) {
      GetDlgItemTextA(param_1, 0x3fc, (LPSTR)&DXDraw::FPSName, 0x3c);
      GetDlgItemTextA(param_1, 0x3fd, (LPSTR)&DXDraw::FPSCaption, 0x3c);
      EndDialog(param_1, -1);
    }
    return 0;
  }
  // WM_INITDIALOG handler
  SetDlgItemTextA(param_1, 0x3fc, &DAT_1001c810);   // set default text
  hWnd = GetDlgItem(param_1, 0x3fc);
  SetFocus(hWnd);
  DAT_1001c04c = 0;
  return 1;
}
```

### Purpose
Standard Win32 dialog procedure for a dialog resource "IDD_DIALOG1" that lets the user enter an FPS display name and caption. On initialization (WM_INITDIALOG), it populates the name field with a default value and sets focus. When the user clicks OK (control 0x3fe / WM_COMMAND), it reads back `DXDraw::FPSName` (control 0x3fc) and `DXDraw::FPSCaption` (control 0x3fd) into their respective globals and closes the dialog. The parent function FUN_10003e80 shows/hides the cursor around the dialog display.

### Calling context
FUN_10003e80 (the caller) would logically be named `DXDraw::ShowFPSDialog` -- it calls `ShowCursor(1)`, invokes this dialog, then `ShowCursor(0)`.

### M2DX.dll Equivalent
Address 0x10003de0 in M2DX.dll falls inside `FUN_10003c10`, a ZBuffer format enumeration function -- completely unrelated. No address correspondence.

---

## 3. FUN_10006e10 -- Proposed name: `DXPlay::ReceiveThreadProc`

**Subsystem:** DXPlay (network multiplayer message receive loop)

### Callers
- No direct CALL references; passed as a LPTHREAD_START_ROUTINE to `CreateThread` in `FUN_10006930` (0x10006930)

### Data reference
- Referenced at 0x1000698c inside FUN_10006930

### Callees
- `WaitForMultipleObjects`, `ExitThread`
- `FUN_10006e70` (0x10006e70) -- the actual message receive/dispatch loop

### Decompiled Code

```c
void DXPlay_ReceiveThreadProc(void)
{
  DWORD DVar1;
  HANDLE local_8;
  undefined4 local_4;

  local_8 = DAT_1004ed08;      // "data ready" event
  local_4 = DAT_1004ed20;      // "shutdown" event

  DVar1 = WaitForMultipleObjects(2, &local_8, 0, INFINITE);
  while (DVar1 == 0) {         // WAIT_OBJECT_0 = data event signaled
    FUN_10006e70();             // process received messages
    DVar1 = WaitForMultipleObjects(2, &local_8, 0, INFINITE);
  }
  // WAIT_OBJECT_0 + 1 = shutdown event, or error -- exit
  ExitThread(0);
}
```

### Purpose
Background thread entry point for DirectPlay message reception. Waits on two event handles: the first (DAT_1004ed08) signals that data is available and triggers message processing via FUN_10006e70; the second (DAT_1004ed20) signals thread shutdown. The wait is non-blocking on the second event (bWaitAll=FALSE), so when the shutdown event fires, the wait returns index 1 and the loop exits, calling ExitThread(0).

FUN_10006e70 (the receive handler) calls through the IDirectPlay4 vtable at offset 0x64 (Receive method) in a loop, dispatching system messages to `FUN_100073e0` and player messages to `FUN_10006f10`, with a "BUFFER_TOO_SMALL" error guard.

### Calling context
FUN_10006930 (the creator) would logically be named `DXPlay::CreateReceiveThread` -- it creates 4 events (data-ready, secondary, tertiary, shutdown) and spawns this thread.

### M2DX.dll Equivalent
Address 0x10006e10 in M2DX.dll falls inside `DXDraw::PrintTGA` -- completely unrelated. No address correspondence.

---

## Summary Table

| Address      | Proposed Name                | Subsystem | Purpose                                          |
|-------------|------------------------------|-----------|--------------------------------------------------|
| 0x100029d0  | `DXDraw::DisplayFrameRate`   | DXDraw    | GDI text overlay of fps/tps/mpps on back buffer  |
| 0x10003de0  | `DXDraw::FPSDialogProc`      | DXDraw    | Dialog callback for FPS name/caption input        |
| 0x10006e10  | `DXPlay::ReceiveThreadProc`  | DXPlay    | Worker thread entry for DirectPlay msg reception  |

### Additional rename suggestions from this analysis

| Address      | Current Name    | Proposed Name                    |
|-------------|-----------------|----------------------------------|
| 0x100029a0  | FUN_100029a0    | `DXDraw::SetFrameRateAndDisplay` |
| 0x10003e80  | FUN_10003e80    | `DXDraw::ShowFPSDialog`          |
| 0x10006930  | FUN_10006930    | `DXPlay::CreateReceiveThread`    |
| 0x10006e70  | FUN_10006e70    | `DXPlay::ReceiveMessages`        |
