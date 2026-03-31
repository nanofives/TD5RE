# Settings.exe - Reverse Engineering Analysis

## Overview

**File:** `Settings.exe`
**Format:** 32-bit PE (x86 LE), compiled with MSVC (Visual C++ Runtime Library)
**Image Base:** `0x00400000`
**Entry Point:** `0x00401cbb` (`entry`)
**Total Functions:** 142 (most are CRT library functions)
**Key Application Functions:** ~10 (the rest are MSVC runtime/CRT)

Settings.exe is a simple Win32 dialog-based application that configures Test Drive 5 game settings. It does **not** use the registry or INI files for persistent storage in the traditional sense. Instead, it:

1. Loads `M2DX.dll` (the game's DirectX framework library) at startup
2. Calls `DXWin::Environment()` to parse command-line/environment settings
3. Presents a dialog box (`DLG_MASTER`) with checkboxes for all options
4. On "Save" (dialog result == 4), writes a `TD5.ini` configuration file

---

## Architecture & Communication Model

### How Settings.exe communicates with TD5_d3d.exe

Settings.exe does **not** launch TD5_d3d.exe directly. Instead, the communication is entirely through:

1. **M2DX.dll shared global structures** - Settings.exe imports global variables from M2DX.dll:
   - `DX::app` (tagDX_APP struct) - application-level settings
   - `DXDraw::dd` (tagDX_dd struct) - DirectDraw settings
   - `DXD3D::d3d` (tagDX_D3D struct) - Direct3D settings
   - `DXInput::FFGainScale` (float) - force feedback gain
   - `DXWin::bLogPrefered` (int) - logging toggle

2. **TD5.ini file** - When the user clicks "Save" (control ID 0x3EF), the dialog returns result code 4 to `WinMain`, which triggers `WriteIniFile()` (FUN_004017f0). This writes a commented configuration file (`TD5.ini`) that TD5_d3d.exe reads on startup via the same `DXWin::Environment()` parser.

### Startup flow

```
entry() [0x00401cbb]
  -> GetVersion(), CRT init, GetCommandLineA(), etc.
  -> WinMain() [0x00401b70]
       -> DXWin::Environment(commandLine)    // parse existing settings from M2DX.dll
       -> DialogBoxParamA("DLG_MASTER", DlgProc_Master)
       -> if (dialog result == 4) -> WriteIniFile()  // write TD5.ini
```

### Settings storage: TD5.ini

The INI file is **not** a standard Windows INI (no `[sections]`, no `key=value`). It is a custom keyword-based text format with comment support (`#`, `rem`, `//`, `;`). Each keyword is either active (uncommented) or disabled (prefixed with `#`).

The file is opened via CRT `fopen()` wrapper at `FUN_00401c58("TD5.ini", mode)`. On first write (`DAT_00409030 == 0`), the mode is `"w"` (create/truncate); subsequent writes append.

---

## Dialog Controls - Complete Map

The dialog resource is named `"DLG_MASTER"`. All controls are checkboxes using `CheckDlgButton` / `IsDlgButtonChecked`.

### 3D Card Setup

| Control ID | Hex ID | Label | M2DX Variable | Offset | Active Value | Inactive Value |
|-----------|--------|-------|---------------|--------|-------------|----------------|
| 1000 | 0x3E8 | Disable intro movie (NoMovie) | `DX::app` | +0x13C | 4 | 0 |
| 1001 | 0x3E9 | Use Primary 3D card | `DXDraw::dd` | +0x169C | 1 | 0 |
| 1002 | 0x3EA | Use 3D card selector (DeviceSelect) | `DXDraw::dd` | +0x169C | -1 (0xFFFFFFFF) | 0 |
| 1003 | 0x3EB | No Multimon Check | `DXDraw::dd` | +0x1670 | 4 | 0 |
| 1004 | 0x3EC | Disable primary test (NoPrimaryTest) | `DXD3D::d3d` | +0xA54 | 1 | 0 |

**Note:** Controls 0x3E9 (Primary) and 0x3EA (DeviceSelect) are mutually exclusive radio-style checkboxes. They both write to `dd+0x169C` but with different values (1 vs -1). Checking one unchecks the other.

### Intro Movie

| Control ID | Hex ID | Label | M2DX Variable | Offset | Active Value | Inactive Value |
|-----------|--------|-------|---------------|--------|-------------|----------------|
| 1005 | 0x3ED | Hardware accelerate movie (FixMovie) | `DX::app` | +0x170 | 1 | 0 |
| 1006 | 0x3EE | Run in Window mode | `DX::app` | +0x150 | 1 | 0 |
| 1009 | 0x3F1 | Disable intro movie (NoMovie) | `DX::app` | +0x174 | 0 (checked=disable) | 1 |

**Note:** Control 0x3F1 (Disable intro movie) is **enabled/disabled** based on control 0x3E8 (NoMovie master toggle). When 0x3E8 is unchecked, 0x3F1 is grayed out. The logic is inverted: checked = `app+0x174 = 0`, unchecked = `app+0x174 = 1`.

### Save / Cancel

| Control ID | Hex ID | Action |
|-----------|--------|--------|
| 1007 | 0x3EF | **Save** - calls `EndDialog(hwnd, 4)` which triggers INI write |
| 1008 | 0x3F0 | **Cancel** - calls `EndDialog(hwnd, 2)` |
| 1, 2 | 0x01, 0x02 | Also mapped to Cancel (IDOK/IDCANCEL standard) |

### Rendering Fixes

| Control ID | Hex ID | Label | M2DX Variable | Offset | Active Value | Inactive Value |
|-----------|--------|-------|---------------|--------|-------------|----------------|
| 1011 | 0x3F3 | Disable 3-buffering (NoTripleBuffer) | `DXDraw::dd` | +0x1674 | 4 | 0 |
| 1012 | 0x3F4 | Disable W-buffering (NoWBuffer) | `DXD3D::d3d` | +0xA30 | 4 | 0 |
| 1013 | 0x3F5 | Disable MipMapping (NoMIP) | `DXD3D::d3d` | +0xA24 | 4 | 0 |
| 1014 | 0x3F6 | Disable AGP textures (NoAGP) | `DXDraw::dd` | +0x167C | 4 | 0 |
| 1020 | 0x3FC | Disable reflections (FixTransparent) | `DXD3D::d3d` | +0xA5C | 1 | 0 |

### Force Feedback Strength

| Control ID | Hex ID | Label | M2DX Variable | Float Value | Hex (IEEE 754) |
|-----------|--------|-------|---------------|-------------|----------------|
| 1015 | 0x3F7 | Weak force feedback | `DXInput::FFGainScale` | 0.5f | 0x3F000000 |
| 1016 | 0x3F8 | Medium force feedback | `DXInput::FFGainScale` | 0.75f | 0x3F400000 |
| 1017 | 0x3F9 | Strong force feedback | `DXInput::FFGainScale` | 1.0f | 0x3F800000 |

These three are mutually exclusive (radio-button behavior). Checking one unchecks the other two.

### Logging

| Control ID | Hex ID | Label | M2DX Variable | Active Value | Inactive Value |
|-----------|--------|-------|---------------|-------------|----------------|
| 1018 | 0x3FA | Enable logging (Log) | `DXWin::bLogPrefered` | 1 | 0 |

---

## TD5.ini File Format

The `WriteIniFile()` function (0x004017f0) writes `TD5.ini` with the following structure:

```
##############################################################################
#                                                                            #
# Test Drive 5 program settings                                              #
#                                                                            #
##############################################################################
#                                                                            #
# The following settings can be made to change the way the program behaves   #
#                                                                            #
# The following characters may be used to indicate comments: //  ;  rem  #   #
# The keywords are not case-sensitive                                        #
#                                                                            #
##############################################################################

[#]Primary
rem If the system contains a Voodoo2 or other separate 3d-only video card the
rem program will choose it. If you would prefer to use your main (primary)
rem video card use this.

[#]DeviceSelect
rem ...

[#]NoMultiMon
[#]NoPrimaryTest
[#]NoMovie
[#]FixMovie
[#]TimeDemo
#FullScreen
[#]Window
[#]NoTripleBuffer
[#]NoWBuffer
[#]NoMIP
[#]NoAGP
[#]WeakForceFeedback
[#]StrongForceFeedback
[#]FixTransparent
[#]Log
```

Each keyword is either written as-is (active) or prefixed with `#` (commented out / inactive). The `[#]` notation above means "conditionally commented". The function checks each M2DX global variable and writes the `#` prefix if the setting is not active.

### INI Keywords to M2DX Variable Mapping

| Keyword | M2DX Struct | Offset | Check Condition (keyword active when) |
|---------|-------------|--------|--------------------------------------|
| `Primary` | `DXDraw::dd` | +0x169C | == 1 |
| `DeviceSelect` | `DXDraw::dd` | +0x169C | == -1 |
| `NoMultiMon` | `DXDraw::dd` | +0x1670 | == 4 |
| `NoPrimaryTest` | `DXD3D::d3d` | +0xA54 | == 1 |
| `NoMovie` | `DX::app` | +0x13C | == 4 |
| `FixMovie` | `DX::app` | +0x174 | == 0 (inverted!) |
| `TimeDemo` | `DX::app` | +0x170 | == 1 |
| `#FullScreen` | - | - | Always commented (default) |
| `Window` | `DX::app` | +0x150 | == 1 |
| `NoTripleBuffer` | `DXDraw::dd` | +0x1674 | == 4 |
| `NoWBuffer` | `DXD3D::d3d` | +0xA30 | == 4 |
| `NoMIP` | `DXD3D::d3d` | +0xA24 | == 4 |
| `NoAGP` | `DXDraw::dd` | +0x167C | == 4 |
| `WeakForceFeedback` | `DXInput::FFGainScale` | - | == 0x3F000000 (0.5f) |
| `StrongForceFeedback` | `DXInput::FFGainScale` | - | == 0x3F800000 (1.0f) |
| `FixTransparent` | `DXD3D::d3d` | +0xA5C | != 0 |
| `Log` | `DXWin::bLogPrefered` | - | != 0 |

---

## M2DX.dll Imported Symbols

Settings.exe imports these from `M2DX.dll`:

| Mangled Name | Demangled | Type | IAT Address |
|-------------|-----------|------|-------------|
| `?app@DX@@2UtagDX_APP@@A` | `DX::app` (tagDX_APP) | Global struct | PTR @ 0x004080C0 |
| `?dd@DXDraw@@2UtagDX_dd@@A` | `DXDraw::dd` (tagDX_dd) | Global struct | PTR @ 0x004080C4 |
| `?d3d@DXD3D@@2UtagDX_D3D@@A` | `DXD3D::d3d` (tagDX_D3D) | Global struct | PTR @ 0x004080C8 |
| `?FFGainScale@DXInput@@2MA` | `DXInput::FFGainScale` | float | PTR @ 0x004080B8 |
| `?bLogPrefered@DXWin@@2HA` | `DXWin::bLogPrefered` | int | PTR @ 0x004080CC |
| `?Environment@DXWin@@SAHPAD@Z` | `DXWin::Environment(char*)` | static function | PTR @ 0x004080BC |

### Recovered Structure Offsets

**tagDX_APP** (`DX::app`):
| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| +0x13C | 4 | noMovie | 0=normal, 4=skip intro movie |
| +0x14C | 4 | fullScreen | 0=windowed (checkbox pre-checked), nonzero=fullscreen |
| +0x150 | 4 | windowMode | 0=normal, 1=run in window |
| +0x170 | 4 | timeDemo | 0=off, 1=performance test mode |
| +0x174 | 4 | fixMovie | 0=hardware accelerate movie, 1=normal |

**tagDX_dd** (`DXDraw::dd`):
| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| +0x1670 | 4 | noMultiMon | 0=normal, 4=disable multimonitor check |
| +0x1674 | 4 | noTripleBuffer | 0=normal, 4=disable triple buffering |
| +0x167C | 4 | noAGP | 0=normal, 4=disable AGP textures |
| +0x169C | 4 | deviceSelection | 0=auto, 1=use primary, -1=device selector |

**tagDX_D3D** (`DXD3D::d3d`):
| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| +0xA24 | 4 | noMipMapping | 0=normal, 4=disable mipmapping |
| +0xA30 | 4 | noWBuffer | 0=normal, 4=disable W-buffer |
| +0xA54 | 4 | noPrimaryTest | 0=normal, 1=disable primary video test |
| +0xA5C | 4 | fixTransparent | 0=normal, 1=fix reflections |

---

## Win32 API Imports (Application-Relevant)

### USER32.DLL
| Function | Usage |
|----------|-------|
| `DialogBoxParamA` | Creates the main settings dialog |
| `CheckDlgButton` | Sets checkbox state during WM_INITDIALOG |
| `IsDlgButtonChecked` | Reads checkbox state on WM_COMMAND |
| `GetDlgItem` | Gets handle to "Disable intro movie" control for enable/disable |
| `EnableWindow` | Enables/disables the "Disable intro movie" checkbox |
| `EndDialog` | Closes dialog with result code (4=save, 2=cancel) |
| `wvsprintfA` | Formats strings for INI file writing |

### KERNEL32.DLL (Application-Relevant)
| Function | Usage |
|----------|-------|
| `GetModuleHandleA` | Gets HINSTANCE for dialog creation |
| `GetCommandLineA` | Gets command line to pass to DXWin::Environment |
| `LoadLibraryA` | Loads M2DX.dll |
| `GetProcAddress` | Resolves M2DX.dll exports |
| `lstrlenA` | String length for INI file buffer management |
| `CreateFileA` | File I/O for TD5.ini (via CRT wrappers) |

**Notable absence:** No `RegSetValueEx`, `RegQueryValueEx`, `RegOpenKeyEx`, `RegCreateKeyEx`, or any registry APIs. Settings.exe does **not** use the Windows registry at all.

---

## Complete Function List with Proposed Names

### Application Functions (custom code)

| Address | Original Name | Proposed Name | Description |
|---------|--------------|---------------|-------------|
| 0x00401000 | FUN_00401000 | `WriteIniLine` | Writes a formatted line to TD5.ini. Varargs, uses wvsprintfA. |
| 0x004010A0 | (created) | `DlgProc_Master` | Main dialog procedure for DLG_MASTER. Handles WM_INITDIALOG (0x110) and WM_COMMAND (0x111). |
| 0x004017F0 | FUN_004017f0 | `WriteIniFile` | Writes complete TD5.ini file with all settings and comments. |
| 0x00401B70 | FUN_00401b70 | `WinMain` | Application main: calls DXWin::Environment, shows dialog, triggers save. |
| 0x00401BB0 | FUN_00401bb0 | `fclose_wrapper` | CRT fclose implementation. |
| 0x00401C06 | FUN_00401c06 | `fputs_wrapper` | CRT fputs - writes string to file stream. |
| 0x00401C38 | FUN_00401c38 | `fopen_ex` | CRT fopen with explicit mode flags. |
| 0x00401C58 | FUN_00401c58 | `fopen_wrapper` | Thin wrapper around fopen_ex with default flags (0x40). |
| 0x00401C6B | FUN_00401c6b | `CRT_init_float` | CRT floating-point initialization. |
| 0x00401C83 | FUN_00401c83 | `CRT_init_float_pointers` | Sets up floating-point conversion function pointers. |
| 0x00401CBB | entry | `entry` / `_mainCRTStartup` | CRT entry point: OS version detection, heap init, calls WinMain. |

### CRT / Runtime Library Functions

| Address | Original/Known Name | Proposed Name | Description |
|---------|-------------------|---------------|-------------|
| 0x00401DB1 | __amsg_exit | `__amsg_exit` | CRT fatal error handler |
| 0x00401DD6 | FUN_00401dd6 | `_abort_init` | Calls SetLastError-style abort |
| 0x00401DFA | FUN_00401dfa | `_free` | CRT free() |
| 0x00401E29 | FUN_00401e29 | `_close` | CRT close file descriptor |
| 0x00401EDC | __freebuf | `__freebuf` | Free file stream buffer |
| 0x00401F07 | FUN_00401f07 | `_fflush` | CRT fflush |
| 0x00401F42 | FUN_00401f42 | `_flush_stream` | Flush a single stream |
| 0x00401FA7 | flsall | `flsall` | Flush all streams |
| 0x00402014 | FUN_00402014 | `_lock_file` | Lock FILE for thread safety |
| 0x004020A1 | FUN_004020a1 | `_unlock_file` | Unlock FILE |
| 0x004020DE | FUN_004020de | `_output` | CRT formatted output core (printf engine) |
| 0x0040281F | FUN_0040281f | `_write_char` | Write single char to stream |
| 0x00402854 | FUN_00402854 | `_write_string` | Write string to stream |
| 0x00402885 | FUN_00402885 | `_write_nchars` | Write N chars to stream |
| 0x004028BD | FUN_004028bd | `_get_stream_flags` | Get stream flags |
| 0x004028CA | FUN_004028ca | `_get_stream_pos` | Get stream position |
| 0x004028DA | FUN_004028da | `_get_stream_count` | Get stream char count |
| 0x004028E8 | FUN_004028e8 | `_openfile` | CRT file open core |
| 0x00402A58 | FUN_00402a58 | `_getstream` | Allocate a FILE stream |
| 0x00402AD0 | FUN_00402ad0 | `_initterm_callback` | CRT initializer callback |
| 0x00402AE2 | FUN_00402ae2 | `_initterm` | CRT initialization terminator |
| 0x00402B20 | FUN_00402b20 | `_cinit` | CRT C initialization |
| 0x00402B49 | FUN_00402b49 | `_cropzeros` | Remove trailing zeros from float string |
| 0x00402C09 | __fassign | `__fassign` | Float assignment from string |
| 0x00402C47 | FUN_00402c47 | `_FLOAT_ecvt` | Float to E-format conversion |
| 0x00402D4B | FUN_00402d4b | `_FLOAT_fcvt` | Float to F-format conversion |
| 0x00402E29 | FUN_00402e29 | `_FLOAT_gcvt_ext` | Float to G-format extended |
| 0x00402EC4 | FUN_00402ec4 | `_FLOAT_ecvt_ext` | Float to E-format extended |
| 0x00402EEB | FUN_00402eeb | `_FLOAT_fcvt_ext` | Float to F-format extended |
| 0x00402F0E | __cfltcvt | `__cfltcvt` | CRT float conversion dispatch |
| 0x00402F5F | FUN_00402f5f | `_shift_text` | Shift text in buffer |
| 0x00402F84 | FUN_00402f84 | `_setenvp` | Set environment pointer |
| 0x00402FB1 | FUN_00402fb1 | `_exit_wrapper` | Calls __exit |
| 0x00402FC2 | __exit | `__exit` | CRT exit |
| 0x00402FD3 | FUN_00402fd3 | `_doexit` | CRT exit processing |
| 0x0040306C | FUN_0040306c | `_initterm_e` | CRT error-checking initializer |
| 0x00403086 | FUN_00403086 | `_XcptFilter` | CRT exception filter |
| 0x004031C7 | FUN_004031c7 | `_ioinit_stream` | I/O stream init |
| 0x0040320A | FUN_0040320a | `_GetCommandLineContents` | Parse command line into args |
| 0x00403262 | FUN_00403262 | `_setargv` | CRT argument vector setup |
| 0x0040331B | FUN_0040331b | `_setenvp_full` | Full environment setup |
| 0x004033B4 | FUN_004033b4 | `_parse_cmdline` | Command line parser |
| 0x00403568 | FUN_00403568 | `_crt_get_environ` | Get environment strings |
| 0x0040369A | FUN_0040369a | `_heap_init` | Heap initialization |
| 0x00403845 | FUN_00403845 | `_mtinitlocks` | Multi-thread lock init |
| 0x00403884 | __global_unwind2 | `__global_unwind2` | SEH global unwind |
| 0x004038C6 | __local_unwind2 | `__local_unwind2` | SEH local unwind |
| 0x0040395A | FUN_0040395a | `_SEH_prolog` | SEH prolog |
| 0x00403A39 | FUN_00403a39 | `_abnormal_termination` | SEH abnormal termination check |
| 0x00403A54 | FUN_00403a54 | `_SEH_epilog` | SEH epilog |
| 0x00403A8D | FUN_00403a8d | `_except_handler3` | SEH exception handler |
| 0x00403BE0 | FUN_00403be0 | `_ioinit` | I/O subsystem init |
| 0x00403C1E | FUN_00403c1e | `_isatty` | Check if fd is terminal |
| 0x00403C49 | FUN_00403c49 | `_write` | CRT write to fd |
| 0x00403F74 | FUN_00403f74 | `_read` | CRT read from fd |
| 0x0040427D | FUN_0040427d | `_ioinit_handles` | Init standard handles |
| 0x0040432E | FUN_0040432e | `_osfhnd_get` | Get OS file handle |
| 0x00404429 | FUN_00404429 | `_set_osfhnd` | Set OS file handle |
| 0x00404490 | FUN_00404490 | `_alloc_osfhnd` | Allocate OS file handle |
| 0x00404525 | FUN_00404525 | `_free_osfhnd` | Free OS file handle |
| 0x0040459C | FUN_0040459c | `_lock` | CRT lock |
| 0x00404616 | FUN_00404616 | `_unlock` | CRT unlock |
| 0x00404653 | FUN_00404653 | `_lock_init` | Initialize lock |
| 0x004046AA | FUN_004046aa | `_dosmaperr` | Map OS error to errno |
| 0x00404910 | _malloc | `_malloc` | CRT malloc |
| 0x00404922 | __nh_malloc | `__nh_malloc` | No-throw malloc |
| 0x0040494E | FUN_0040494e | `_heap_alloc` | Heap allocator |
| 0x00404984 | FUN_00404984 | `_isdigit` | Character classification |
| 0x004049B0 | _strlen | `_strlen` | String length |
| 0x00404A2B | FUN_00404a2b | `_wctomb` | Wide char to multibyte |
| 0x00404AA0 | __aulldiv | `__aulldiv` | Unsigned 64-bit division |
| 0x00404B10 | __aullrem | `__aullrem` | Unsigned 64-bit remainder |
| 0x00404B85 | FUN_00404b85 | `_strtol_helper` | String to long helper |
| 0x00404C9A | FUN_00404c9a | `_strtol` | String to long |
| 0x00404F53 | FUN_00404f53 | `_realloc_helper` | Realloc helper |
| 0x00404F88 | FUN_00404f88 | `_realloc` | CRT realloc |
| 0x00404F9E | FUN_00404f9e | `_heap_free` | Heap free |
| 0x00405030 | FUN_00405030 | `_heap_realloc` | Heap realloc |
| 0x004050B9 | FUN_004050b9 | `_heap_expand` | Heap expand |
| 0x0040512E | FUN_0040512e | `_heap_shrink` | Heap shrink |
| 0x004051F9 | FUN_004051f9 | `_lseeki64` | 64-bit file seek |
| 0x00405242 | FUN_00405242 | `_lseek` | File seek |
| 0x00405298 | FUN_00405298 | `_lseeki64_nolock` | Seek without lock |
| 0x00405324 | FUN_00405324 | `_ftell_helper` | ftell implementation |
| 0x0040533F | FUN_0040533f | `_fseek_helper` | fseek implementation |
| 0x0040534B | FUN_0040534b | `_ftell` | CRT ftell |
| 0x00405366 | FUN_00405366 | `_fp_mult` | Floating point multiply |
| 0x004053F3 | FUN_004053f3 | `_fp_div` | Floating point divide |
| 0x0040555F | FUN_0040555f | `_fp_add` | Floating point add |
| 0x00405575 | FUN_00405575 | `_fp_sub` | Floating point subtract |
| 0x0040558B | FUN_0040558b | `_fp_shl` | Floating point shift left |
| 0x004055B8 | FUN_004055b8 | `_fp_shr` | Floating point shift right |
| 0x004055F0 | FUN_004055f0 | `_fp_copy` | Floating point copy |
| 0x00405600 | FUN_00405600 | `_fp_normalize` | Floating point normalize |
| 0x004056E0 | FUN_004056e0 | `_fp_tostring` | Float to string |
| 0x00405757 | FUN_00405757 | `_fp_init` | Float subsystem init |
| 0x004057BB | FUN_004057bb | `_fp_compare` | Float compare |
| 0x00405880 | _memset | `_memset` | CRT memset |
| 0x004058E0 | FUN_004058e0 | `_memcpy` | CRT memcpy |
| 0x00405C15 | FUN_00405c15 | `_ctype_init` | Character type init |
| 0x00405C1E | FUN_00405c1e | `_tolower` | Character to lowercase |
| 0x00405C2F | FUN_00405c2f | `_toupper` | Character to uppercase |
| 0x00405C60 | FUN_00405c60 | `_setlocale` | Set locale |
| 0x00405DF9 | FUN_00405df9 | `_getlocaleinfo` | Get locale info |
| 0x00405E43 | FUN_00405e43 | `_setmbcp` | Set multibyte codepage |
| 0x00405E76 | FUN_00405e76 | `_mbctype_init` | Multibyte char type init |
| 0x00405E9F | FUN_00405e9f | `_setmbcp_full` | Full MB codepage setup |
| 0x00406024 | FUN_00406024 | `_mbctype_set` | Set MB char types |
| 0x00406040 | FUN_00406040 | `_memmove` | CRT memmove |
| 0x00406375 | FUN_00406375 | `_memcmp` | CRT memcmp |
| 0x00406400 | _strncpy | `_strncpy` | CRT strncpy |
| 0x004064FE | FUN_004064fe | `_lseek_osfhnd` | Seek via OS handle |
| 0x00406598 | FUN_00406598 | `_chsize` | Change file size |
| 0x0040666D | FUN_0040666d | `_fileno` | Get file descriptor |
| 0x00406688 | FUN_00406688 | `_fstat_init` | File stat initialization |
| 0x004066CC | FUN_004066cc | `_ismbblead` | Is multibyte lead byte |
| 0x00406812 | FUN_00406812 | `_strcoll` | String collation |
| 0x00406A08 | FUN_00406a08 | `_getlocaleinfo_ex` | Extended locale info |
| 0x00406B51 | FUN_00406b51 | `_widechar_convert` | Wide character conversion |
| 0x00406D75 | FUN_00406d75 | `_mbstowcs_helper` | Multibyte to wide helper |
| 0x00406DA0 | FUN_00406da0 | `_fp_shift` | Floating point shift |
| 0x00406DC1 | ___add_12 | `___add_12` | 12-byte float add |
| 0x00406E1F | FUN_00406e1f | `_fp_negate` | Float negate |
| 0x00406E4D | FUN_00406e4d | `_fp_inc` | Float increment |
| 0x00406E7A | FUN_00406e7a | `_fp_from_string` | Float from string |
| 0x00406F41 | FUN_00406f41 | `_fp_format` | Float format engine |
| 0x00407412 | FUN_00407412 | `_fp_round` | Float rounding |
| 0x004076A5 | FUN_004076a5 | `_fp_digits` | Float digit extraction |
| 0x00407720 | FUN_00407720 | `_fp_init_tables` | Float table initialization |
| 0x0040774F | FUN_0040774f | `_fp_multiply_big` | Big float multiply |
| 0x0040796F | FUN_0040796f | `_fp_add_big` | Big float add |
| 0x004079EC | RtlUnwind | `RtlUnwind` (thunk) | SEH unwind thunk |

---

## Key Findings Summary

1. **No registry access.** Settings.exe stores everything in `TD5.ini`, a custom keyword-based config file.

2. **No command-line building.** Settings.exe does not launch TD5_d3d.exe. It only writes `TD5.ini`. The game executable (TD5_d3d.exe) reads `TD5.ini` on startup via the shared `DXWin::Environment()` parser in M2DX.dll.

3. **M2DX.dll is the shared state layer.** Both Settings.exe and TD5_d3d.exe link against M2DX.dll and share the same global structures (`DX::app`, `DXDraw::dd`, `DXD3D::d3d`, `DXInput::FFGainScale`, `DXWin::bLogPrefered`). Settings.exe calls `DXWin::Environment()` at startup to populate these structs from the existing TD5.ini, presents them to the user via the dialog, then writes them back.

4. **Magic value 4 is used as a "forced/override" flag** for many boolean settings (NoTripleBuffer, NoWBuffer, NoMIP, NoAGP, NoMultiMon, NoMovie). This likely means "user explicitly set this" vs 0 meaning "default/auto".

5. **Force feedback uses IEEE 754 float values** stored as raw integers: 0.5 (weak), 0.75 (medium), 1.0 (strong). The default (medium) corresponds to no WeakForceFeedback or StrongForceFeedback keyword being active in TD5.ini.

6. **The dialog has 18 checkbox controls** plus Save/Cancel buttons, all in a single flat dialog (no tabs or property sheets).

7. **FixMovie / NoMovie interaction:** Control 0x3F1 (Disable intro movie) is only enabled when control 0x3E8 (the master NoMovie toggle) is checked. This allows the user to choose between "no movie" and "hardware accelerated movie".
