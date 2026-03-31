# M2DXFX.dll -- Comprehensive Analysis

> Binary: `M2DXFX.dll` (151,552 bytes)
> Ghidra port: 8194
> Base address: `0x10000000`
> Sections: `.text` (0x10001000, 90112B), `.rdata` (0x10017000, 20480B), `.data` (0x1001c000, 227164B), `.reloc` (0x10054000, 12288B), `tdb`
> Functions: 406 total | Exports: 474 (164 unique ordinals) | Imports: 132

## Identity and Purpose

**M2DXFX.dll is NOT an FMV/movie subsystem.** Despite the "FX" suffix suggesting special effects or FMV, the DLL is a **stripped-down variant of M2DX.dll** that provides the same DirectX engine subsystems minus the Direct3D rendering layer. It is the **non-3D-accelerated build** of the M2DX engine library.

### Comparison with M2DX.dll

| Property | M2DX.dll | M2DXFX.dll |
|---|---|---|
| File size | 184,320 B | 151,552 B |
| .text size | 114,688 B | 90,112 B |
| Export count | 564 | 474 |
| DXD3D class | YES (BeginScene, EndScene, FullScreen, ChangeDriver, TextureClamp, etc.) | **NO** |
| DXD3DTexture class | YES (Load, LoadRGB, Manage, RestoreAll, etc.) | **NO** |
| DXDraw class | YES | YES (identical API) |
| DXInput class | YES | YES (identical API) |
| DXSound class | YES | YES (identical API) |
| DXPlay class | YES | YES (identical API) |
| DXWin class | YES | YES (identical API) |
| DX utilities | YES | YES (identical API) |
| Display mode enum | Has 4:3 aspect filter | **No 4:3 filter** (simpler EnumDisplayModes path) |

### Key Difference: DXWin::DXInitialize

In M2DX.dll, `DXInitialize` calls `DXDraw::Create`, `DXInput::Create`, `DXSound::Create`, `DXSound::CanDo3D`, **plus** `DXD3D::Create`.
In M2DXFX.dll (0x1000CE40), `DXInitialize` calls only `DXDraw::Create`, `DXInput::Create`, `DXSound::Create`, `DXSound::CanDo3D` -- **no D3D initialization**.

## How It Integrates with the Engine

### Not Used at Runtime

The EXE (`TD5_d3d.exe`) imports only from `M2DX.dll` (string at `0x00460AB0`). **M2DXFX.dll is never loaded by the EXE.** It ships on the CD and is present in the installer data (`TD5.rep/idata/`) but is not part of the active runtime.

### Probable Original Purpose

M2DXFX.dll was likely the engine library used for:
1. **The installer/setup program** -- the setup needs DirectDraw for splash screens and audio for background music, but does NOT need Direct3D.
2. **A software-rendering fallback path** -- a build for systems without 3D acceleration, used during pre-game screens (legal notices, intro movie playback) where only 2D DirectDraw is needed.
3. **CD audio streaming** -- contains the WV2WAV codec installer for CD audio track playback.

## Export Table (164 Unique Functions)

### DXDraw (Display Surface Management)
| Address | Export Name | Purpose |
|---|---|---|
| `0x10001000` | `DXDraw::Environment` | Initialize DXDraw state, zero dd struct, confirm DX6 |
| `0x10001040` | `DXDraw::Create` | Create DirectDraw object, enumerate modes, set cooperative level |
| `0x10001540` | `DXDraw::Destroy` | Release all DirectDraw objects |
| `0x10001600` | `DXDraw::ResetFrameRate` | Reset FPS counter |
| `0x10001640` | `DXDraw::Pause` | Pause/resume rendering |
| `0x10001670` | `DXDraw::CalculateFrameRate` | Compute FPS stats |
| `0x10001740` | `DXDraw::Print` | Print text to info surface |
| `0x10001840` | `DXDraw::PrintTGA` | Render TGA image to surface |
| `0x10001AE0` | `DXDraw::GammaControl` | Set gamma ramp (R, G, B floats) |
| `0x10001BE0` | `DXDraw::Flip` | Present back buffer (Blt in windowed, Flip in fullscreen) |
| `0x10002320` | `DXDraw::ClearBuffers` | Clear back buffer |
| `0x10002440` | `DXDraw::GetDDSurfaceDesc` | Get surface description |
| `0x10002F50` | `DXDraw::CreateDDSurface` | Create arbitrary DD surface |
| `0x10003140` | `DXDraw::CreateSurfaces` | Create front/back buffer surfaces for given W/H |
| `0x10003570` | `DXDraw::DestroySurfaces` | Release front/back/z buffer surfaces |
| `0x10003EC0` | `DXDraw::ConfirmDX6` | Verify DirectX 6.0+ is installed |
| `0x10003F60` | `DXDraw::GetDDObject` | Get IDirectDraw4, front surface, back surface pointers |

### DXDraw Exported Data
| Address | Symbol | Type |
|---|---|---|
| `0x100214F0` | `DXDraw::dd` (tagDX_dd) | DirectDraw state struct |
| `0x10023028` | `DXDraw::ScreenY` | Current screen height (int) |
| `0x10023030` | `DXDraw::ScreenX` | Current screen width (int) |
| `0x1002302C` | `DXDraw::bChrOvrPrn` | Character overprint flag |
| `0x10023638` | `DXDraw::FPSCaption` | FPS display caption string ptr |
| `0x10023DAC` | `DXDraw::FPSName` | FPS display name string ptr |

### DXInput (Keyboard/Joystick/Force Feedback)
| Address | Export Name | Purpose |
|---|---|---|
| `0x10003FE0` | `DXInput::Environment` | Initialize input state |
| `0x10004030` | `DXInput::Create` | Create DirectInput objects, enumerate joysticks |
| `0x100041C0` | `DXInput::Destroy` | Release DirectInput objects |
| `0x10004290` | `DXInput::GetKB` | Read keyboard state |
| `0x100042D0` | `DXInput::CheckKey` | Check if specific key is pressed |
| `0x100042F0` | `DXInput::GetMouse` | Read mouse state |
| `0x10004430` | `DXInput::Pause` | Pause/resume input acquisition |
| `0x10004480` | `DXInput::SetConfiguration` (2-param) | Set key binding |
| `0x10004630` | `DXInput::SetConfiguration` (0-param) | Apply configuration |
| `0x10004640` | `DXInput::ResetConfiguration` | Reset to defaults |
| `0x10004650` | `DXInput::PlayEffect` (3-param) | Play force feedback effect |
| `0x100046F0` | `DXInput::PlayEffect` (4-param) | Play force feedback with extra param |
| `0x100047A0` | `DXInput::StopEffects` | Stop all force feedback |
| `0x100047E0` | `DXInput::SetEffect` | Configure a force feedback effect |
| `0x10004860` | `DXInput::EnumerateEffects` | Enumerate FF effects on device |
| `0x10004D20` | `DXInput::GetJS` | Read joystick axis |
| `0x10004F20` | `DXInput::GetKBStick` | Read keyboard as joystick |
| `0x10005130` | `DXInput::GetKBCursorKey` | Read cursor keys |
| `0x100051C0` | `DXInput::WriteOpen` | Open replay write file |
| `0x100051E0` | `DXInput::Write` | Write replay data |
| `0x100052C0` | `DXInput::WriteClose` | Close replay file |
| `0x100052E0` | `DXInput::ReadOpen` | Open replay read file |
| `0x10005300` | `DXInput::Read` | Read replay data |
| `0x100053C0` | `DXInput::LoadComplete` / `DXInput::ReadClose` | Close replay read (shared ordinal) |
| `0x100053D0` | `DXInput::GetString` | Get key name string |
| `0x100055A0` | `DXInput::SetAnsi` | Enable ANSI key capture |
| `0x100055D0` | `DXInput::GetAnsiKey` | Get ANSI keypress |

### DXInput Exported Data
| Address | Symbol |
|---|---|
| `0x1004B1C0` | `DXInput::js` (joystick info array) |
| `0x1004B2A8` | `DXInput::JoystickType` |
| `0x1004B2C4` | `DXInput::JoystickButtonC` |
| `0x100240A4` | `DXInput::JoystickC` (joystick count) |
| `0x1002409C` | `DXInput::JoystickCurrent` |
| `0x100240A0` | `DXInput::AnsiReadP` |
| `0x1004B30C` | `DXInput::AnsiP` |
| `0x1004B314` | `DXInput::AnsiBuffer` |
| `0x1004B1B8` | `DXInput::bAnsi` |
| `0x1004B1B4` | `DXInput::CreateEffects` (callback ptr) |
| `0x1004B514` | `DXInput::Configure` (16x16 key binding table) |
| `0x1004B628` | `DXInput::FFGainScale` (force feedback gain) |

### DXPlay (DirectPlay Networking)
| Address | Export Name | Purpose |
|---|---|---|
| `0x10005600` | `DXPlay::Environment` | Initialize network state |
| `0x10005630` | `DXPlay::Create` | Create DirectPlay object |
| `0x10005690` | `DXPlay::Destroy` / `DXPlay::Lobby` | Destroy session / Check lobby (shared ordinal) |
| `0x100056B0` | `DXSound::CanDo3D` / `DXPlay::Lobby` | Check 3D sound / lobby (shared ordinal) |
| `0x100056C0` | `DXPlay::ConnectionEnumerate` | Enumerate service providers |
| `0x10005840` | `DXPlay::ConnectionPick` | Select provider by index |
| `0x10005920` | `DXPlay::NewSession` | Create network game session |
| `0x10005AB0` | `DXPlay::JoinSession` | Join existing session |
| `0x10005C20` | `DXPlay::SendMessageA` | Send game message |
| `0x10005F00` | `DXPlay::ReceiveMessage` | Receive game message |
| `0x10006000` | `DXPlay::HandlePadHost` | Host-side gamepad sync |
| `0x10006230` | `DXPlay::HandlePadClient` | Client-side gamepad sync |
| `0x100064C0` | `DXPlay::EnumerateSessions` | Find available sessions |
| `0x100065C0` | `DXPlay::SealSession` | Lock session (no more joins) |
| `0x10006620` | `DXPlay::EnumSessionTimer` | Timer for session enumeration |
| `0x10006680` | `DXPlay::UnSync` | Force desync recovery |

### DXPlay Exported Data
| Address | Symbol |
|---|---|
| `0x1004B648` | `DXPlay::bConnectionLost` |
| `0x1004F14C` | `DXPlay::bEnumerateSessionBusy` |
| `0x1004F1D8` | `DXPlay::dpu` (tagDX_DPU struct) |

### DXSound (DirectSound + CD Audio)
| Address | Export Name | Purpose |
|---|---|---|
| `0x100077E0` | `DXSound::Environment` | Initialize sound state |
| `0x10007870` | `DXSound::Create` | Create DirectSound, install WV2 codec |
| `0x10007990` | `DXSound::Destroy` | Release all sound objects |
| `0x100079D0` | `DXSound::GetDSObject` | Get IDirectSound ptr |
| `0x100079F0` | `DXSound::SetPlayback` | Set playback mode |
| `0x10007A00` | `DXSound::Load` (3-param) | Load WAV file to slot |
| `0x10007A20` | `DXSound::Load` (4-param) | Load WAV with flags |
| `0x10007BA0` | `DXSound::LoadBuffer` (3-param) | Load from memory buffer |
| `0x10007BC0` | `DXSound::LoadBuffer` (4-param) | Load buffer with flags |
| `0x10007CD0` | `DXSound::Play` (1-param) | Play sound slot |
| `0x10007DC0` | `DXSound::Play` (4-param) | Play with position/volume/pan |
| `0x10007EB0` | `DXSound::ModifyOveride` | Modify playing sound (forced) |
| `0x10007F40` | `DXSound::Modify` | Modify playing sound (soft) |
| `0x10007FE0` | `DXSound::Remove` | Remove sound from slot |
| `0x10008080` | `DXSound::Stop` | Stop playing sound |
| `0x100080C0` | `DXSound::Status` | Query sound status |
| `0x10008110` | `DXSound::MuteAll` | Mute all sounds |
| `0x10008150` | `DXSound::UnMuteAll` | Unmute all sounds |
| `0x100081A0` | `DXSound::SetVolume` | Set master volume |
| `0x100081E0` | `DXSound::GetVolume` | Get master volume |
| `0x100081F0` | `DXSound::CDPlay` | Play CD audio track |
| `0x10008430` | `DXSound::CDStop` | Stop CD audio |
| `0x10008500` | `DXSound::CDReplay` | Replay current CD track |
| `0x10008520` | `DXSound::CDSetVolume` | Set CD volume |
| `0x100085B0` | `DXSound::CDGetVolume` | Get CD volume |
| `0x100085C0` | `DXSound::CDPause` | Pause CD audio |
| `0x10008600` | `DXSound::CDResume` | Resume CD audio |
| `0x10008630` | `DXSound::PlayStream` | Play WAV stream from file |
| `0x10008710` | `DXSound::StopStream` | Stop WAV stream |
| `0x10008770` | `DXSound::Refresh` | Pump streaming audio buffer |

### DXWin (Window/Application Management)
| Address | Export Name | Purpose |
|---|---|---|
| `0x1000C790` | `DXWin::Environment` | Master init: zeros app, calls all Environment funcs, parses cmdline |
| `0x1000CC70` | `DXWin::Initialize` | Create window, register class, show cursor |
| `0x1000CE30` | `DXWin::Uninitialize` | Calls CleanUpAndPostQuit |
| `0x1000CE40` | `DXWin::DXInitialize` | Create DXDraw + DXInput + DXSound (NO DXD3D) |
| `0x1000CE70` | `DXWin::CleanUpAndPostQuit` | Destroy all subsystems, post WM_QUIT |
| `0x1000D010` | `DXWin::Pause` | Pause/resume the DXWin message loop |

### DXWin Exported Data
| Address | Symbol |
|---|---|
| `0x10051FD0` | `DX::app` (tagDX_APP struct, 400 bytes) |
| `0x10052174` | `DXWin::bLogPrefered` |

### DX Utility Functions
| Address | Export Name | Purpose |
|---|---|---|
| `0x10008D30` | `DXGethWndMain` | Return main window handle |
| `0x10008D40` | `DX::ExtractSystemInfo` | Gather CPU/OS/RAM info |
| `0x10008F20` | `DX::ExtractScreenInfo` | Enumerate display adapters |
| `0x10009330` | `DX::ImageProBMP` | Load BMP image |
| `0x100093D0` | `DX::ImageProRGB` | Load RGB image |
| `0x10009500` | `DX::ImageProRGBS` | Load RGBS image |
| `0x10009560` | `DX::ImageProRGBS24` | Load RGBS 24-bit image |
| `0x100095B0` | `DX::ImageProRGBS32` | Load RGBS 32-bit image |
| `0x10009600` | `DX::ImageProTGA` | Load TGA image |
| `0x1000B290` | `DX::BitCount` | Count set bits in bitmask |
| `0x1000B2C0` | `DX::TGACompress` | Compress TGA with RLE |
| `0x1000B3A0` | `DX::GetSubString` | String tokenizer |
| `0x1000B4D0` | `DX::FOpen` | File open |
| `0x1000B550` | `DX::FClose` | File close |
| `0x1000B570` | `DX::FRead` | File read |
| `0x1000B590` | `DX::FWrite` | File write |
| `0x1000B5C0` | `DX::FSeek` | File seek |
| `0x1000B620` | `DX::FSize` | File size |
| `0x1000B670` | `DX::Allocate` | Heap allocate |
| `0x1000B680` | `DX::DeAllocate` | Heap free |
| `0x1000B690` | `DX::DXHex` | Format hex string |
| `0x1000B6B0` | `DX::DXDecimal` | Format decimal string |
| `0x1000B6D0` | `DX::DXFDecimal` | Format float decimal string |
| `0x1000B720` | `DX::Report` | Log report message |
| `0x1000B730` | `DX::Msg` | Display error message box |
| `0x1000B790` | `DX::ReleaseMsg` | Display release-mode message |
| `0x1000B830` | `DX::LogReport` | Write to TD5.log |
| `0x1000B8F0` | `DX::GetStateString` | Convert error code to string |
| `0x1000B9A0` | `DX::DXErrorToString` | Full DD/DI/DP error decoder |

### DX Utility Exported Data
| Address | Symbol |
|---|---|
| `0x100511C0` | `DX::Image` (tagDXIMAGELINE) |
| `0x10051220` | `DX::FileENP` |
| `0x100518A0` | `DX::FCount` |
| `0x10051928` | `DX::info` (tagDX_INFO) |
| `0x10052160` | `DX::LastError` |
| `0x10052164` | `DX::MessageC` |
| `0x10052168` | `ErrorN` |
| `0x10052178` | `CheckOut` |
| `0x1005217C` | `CheckOutS` |

## Command-Line Option Table

The DXWin command-line parser (`ApplyStartupOptionToken`, 0x1000CA20) uses a 26-entry string pointer table at `0x10020690`. Options are matched case-insensitively by prefix. Both `TD5.ini` and command-line args are parsed.

| Index | String | Global Written | Value | Notes |
|---|---|---|---|---|
| 0-4 | `//`, `rem`, `;`, `\`, `#` | (none) | -- | Comment tokens (cases 0-4 return 0) |
| 5 | `Log` | `DXWin::bLogPrefered` (0x10052174) | 1 | Enable TD5.log output |
| 6 | `TimeDemo` | `DAT_10052140` | 1 | Show DX6 required dialog on startup |
| 7 | `FullScreen` | `DAT_1005211C` | 1 | Force fullscreen mode |
| 8 | `Window` | `DAT_1005211C` | 0 | Force windowed mode |
| 9 | `NoTripleBuffer` | `DAT_10022B64` | 4 | Disable triple buffering |
| 10 | `DoubleBuffer` | `DAT_10022B64` | 4 | Force double buffering (same effect) |
| 11 | `NoWBuffer` | -- | -- | NOP (fall-through) |
| 12 | `Primary` | `DAT_10022B8C` | 1 | Use primary display device |
| 13 | `DeviceSelect` | `DAT_10022B8C` | -1 | Interactive device selection |
| **14** | **`NoMovie`** | **`DAT_1005210C`** | **4** | **Disable FMV playback** |
| 15 | `FixTransparent` | -- | -- | NOP (fall-through) |
| **16** | **`FixMovie`** | **`DAT_10052144`** | **1** | **Apply movie playback fix** |
| 17 | `NoAGP` | `DAT_10022B6C` | 4 | Disable AGP textures |
| 18 | `NoPrimaryTest` | -- | -- | NOP (fall-through) |
| 19 | `NoMultiMon` | `DAT_10022B60` | 4 | Disable multi-monitor support |
| 20 | `NoMIP` | -- | -- | NOP (fall-through) |
| 21 | `NoLODBias` | -- | -- | NOP (fall-through) |
| 22 | `NoZBias` | -- | -- | NOP (fall-through) |
| 23 | `MatchBitDepth` | `DAT_10022C40` | 1 | Match desktop bit depth |
| 24 | `WeakForceFeedback` | `DXInput::FFGainScale` (0x1004B628) | 0.5 | Half-strength FF |
| 25 | `StrongForceFeedback` | `DXInput::FFGainScale` (0x1004B628) | 1.0 | Full-strength FF |

### NoMovie / FixMovie Flag Analysis

These flags are ONLY WRITTEN inside M2DXFX.dll (and also in M2DX.dll's equivalent parser at `ApplyStartupOptionToken`). They are NEVER READ within either DLL.

In M2DX.dll:
- NoMovie writes to `0x10061C0C` (confirmed: app struct offset)
- FixMovie writes to `0x10061C44` (confirmed: app struct offset)

In M2DXFX.dll:
- NoMovie writes to `0x1005210C`
- FixMovie writes to `0x10052144`

These are offsets within the `DX::app` (tagDX_APP) struct. The EXE reads these at runtime through the imported `app` symbol. In the EXE's `RunMainGameLoop`:
- `app+0xD8` (if != 0): movie playback is attempted
- `app+0xC8` (if == 0): additional guard for movie
- `app+0x10C` (if != 0): **NoMovie flag -- skip movie entirely**
- `app+0xC4`: callback pointer (set to `RequestIntroMovieShutdown`)

## Internal Functions (Renamed in Ghidra)

### DXDraw Internals
| Address | Name | Purpose |
|---|---|---|
| `0x10001E80` | `GetSystemPalette` | Get system palette entries |
| `0x10001EB0` | `ResetDirtyRects` | Reset dirty region tracking for Flip |
| `0x10001F20` | `CopyRectArray` | Copy array of RECTs |
| `0x10001F70` | `MergeDirtyRects` | Merge dirty regions for optimized Blt |
| `0x100022D0` | `RestoreDisplayMode` | Restore original display mode |
| `0x10002490` | `SetCooperativeLevel` | Set DDraw cooperative level |
| `0x10002550` | `EnumAndStoreCapabilities` | Query HW/HEL caps |
| `0x100026A0` | `EnumAndStoreDisplayModes` | Enumerate supported display modes |
| `0x10002730` | `EnumDisplayModesAndBuildResTable` | Build resolution selection table |
| `0x100029A0` | `SetDisplayModeParams` | Store width/height/bpp before apply |
| `0x10002BA0` | `CreateFrameRateInfoSurface` | Create FPS overlay surface |
| `0x10002CE0` | `CreateModeInfoSurface` | Create mode info overlay surface |
| `0x10002F80` | `DXDraw_PauseResumeRendering` | Pause/resume with ref-counting |
| `0x100030A0` | `SetPaletteEntries` | Apply palette to primary surface |
| `0x100030C0` | `SaveAndApplyPauseGamma` | Save/set palette for pause state |
| `0x100035D0` | `UpdateClientAreaMetrics` | Update window client area dimensions |
| `0x10003650` | `SetDisplayMode` | Call IDirectDraw4::SetDisplayMode |
| `0x100036E0` | `ResizeWindowForMode` | Resize window to match display mode |
| `0x100037C0` | `AttachPaletteToFrontBuffer` | Attach palette object |
| `0x10003800` | `RestoreSurfacesIfLost` | Restore lost front/back/Z surfaces |
| `0x100038F0` | `DXDraw_HandleWindowMessage` | Window message handler for DXDraw |
| `0x10003BB0` | `DXDraw_HandleActivateApp` | WM_ACTIVATEAPP handler |
| `0x10003E80` | `ShowDX6RequiredDialog` | Show "DirectX 6.0 required" dialog |

### DXPlay Internals
| Address | Name | Purpose |
|---|---|---|
| `0x10006690` | `DXPlay_PrepareConnection` | Prepare network connection |
| `0x10006930` | `DXPlay_CreateEventsAndReceiveThread` | Create sync events + receive thread |
| `0x100069D0` | `DXPlay_DestroyThreadAndCloseSession` | Shutdown receive thread, close session |
| `0x10006AA0` | `DXPlay_CompareSessionGUID` | Compare 16-byte session GUID |
| `0x10006D70` | `DXPlay_SendTypedMessage` | Send typed message with header |
| `0x10006E70` | `DXPlay_ReceiveThreadLoop` | Background receive thread main loop |
| `0x10006F10` | `DXPlay_HandleAppMessage` | Process application-level message |
| `0x100073E0` | `DXPlay_HandleSystemMessage` | Process DirectPlay system message |

### DXSound Internals
| Address | Name | Purpose |
|---|---|---|
| `0x10008830` | `ParseWavHeaderAndCreateSoundBuffer` | Parse RIFF/WAVE, create IDirectSoundBuffer |
| `0x10008A00` | `RefillStreamBufferHalf` | Double-buffered streaming audio fill |
| `0x10008B80` | `DXSound_LoadWavDataToBuffer` | Lock buffer, copy WAV PCM data |
| `0x10008C70` | `DXSound_CreateDirectSoundDevice` | Call DirectSoundCreate, set coop level |
| `0x10008CF0` | `DXSound_ReleaseDirectSoundObjects` | Release IDirectSound + primary buffer |

### DXWin Internals
| Address | Name | Purpose |
|---|---|---|
| `0x1000C810` | `ParseCommandLineAndIniFile` | Master parser: tries TD5.ini then cmdline |
| `0x1000C950` | `ParseIniFile` | Parse a single .ini file for options |
| `0x1000CA20` | `ApplyStartupOptionToken` | Match token against 26-entry table, apply |
| `0x1000CED0` | `DXWin_WndProc` | Window procedure (WM_CHAR, WM_CLOSE, timers) |

### Image Processing Internals
| Address | Name | Purpose |
|---|---|---|
| `0x100099A0` | `ImageFill4To4Packed` | Pack 2x4-bit nibbles per byte |
| `0x10009B40` | `ImageFillRawByteCopy` | Raw byte copy with pitch handling |
| `0x10009C60` | `ImageFill8ToRGB16` | 8-bit palette to 16-bit RGB |
| `0x10009D40` | `ImageFill8ToRGB32` | 8-bit palette to 32-bit RGB |
| `0x10009DF0` | `ImageFill8ToRGB16Scaled` | 8-bit to 16-bit with scaling |
| `0x10009F90` | `ImageFillRGB16ToRGB16` | 16-bit to 16-bit with format conversion |
| `0x1000A260` | `ImageFillRGB16ToARGB1555` | 16-bit RGB to ARGB1555 |
| `0x1000A2C0` | `ImageFillRGB24ToRGB16` | 24-bit RGB to 16-bit |
| `0x1000A540` | `ImageFillRGB32ToRGB16` | 32-bit RGBA to 16-bit |
| `0x1000A7C0` | `ImageFillRGB24SToRGB16` | 24-bit RGBS to 16-bit |
| `0x1000A900` | `ImageFillRGB32SToRGB16` | 32-bit RGBS to 16-bit |
| `0x1000AA30` | `ImageFillRLE16ToRGB16` | RLE-compressed 16-bit to 16-bit |
| `0x1000ABE0` | `ImageFillRLE16ToRGB32` | RLE-compressed 16-bit to 32-bit |
| `0x1000ADF0` | `BuildPaletteLookupTable` | Build palette-to-pixel LUT |
| `0x1000AF10` | `ConvertRGBAToBGRA` | Swap R/B channels in 32-bit image |
| `0x1000B150` | `BuildOptimalPaletteForImage` | Generate optimal 256-color palette |
| `0x1000B260` | `CountLeadingZeroBits` | Compute bit shift for pixel mask |

### System Info Internals
| Address | Name | Purpose |
|---|---|---|
| `0x10008FA0` | `FormatLogMessage` | Format and append to log buffer |
| `0x10008FD0` | `CalibrateProcessorClockSpeed` | RDTSC + QPC calibration |
| `0x100090B0` | `GetProcessorNameString` | Get CPU name with clock speed |
| `0x10009140` | `IdentifyCPUModel` | Identify Intel/AMD CPU model from CPUID |
| `0x10009270` | `GetDirectXVersionString` | Query ddraw.dll version info |
| `0x10009300` | `GetOSPlatformString` | Return "Windows 95"/"Windows NT"/etc. |

### WV2WAV Codec / Nok DRM System
| Address | Name | Purpose |
|---|---|---|
| `0x1000D030` | `InstallWv2WavCodecFromCDPath` | Read registry for install source, load WV2 codec from CD |
| `0x1000D1E0` | `CreateNokMediaManager` | Allocate and init Nok media manager COM object |
| `0x1000D280` | `NokMediaManager_Constructor` | Initialize vtable and state |
| `0x1000D2C0` | `NokMediaManager_Destructor` | Release COM interfaces and DLLs |
| `0x1000D490` | `NokMediaManager_IterateAndLoadComponent` | Scan loaded DLLs for matching component |
| `0x1000D530` | `EnsureTrailingBackslash` | Append backslash to path if needed |
| `0x1000DBE0` | `NokMediaManager_FreeAllLoadedLibraries` | FreeLibrary all loaded DLL handles |
| `0x1000DC60` | `NokMediaManager_FindAndVerifyDll` | Find DLL in directory, verify "AMKRamkr" signature |
| `0x1000DDD0` | `NokComponentArray_Clear` | Clear component array |
| `0x1000DE00` | `NokComponentArray_GetCount` | Get count (stride 0x210 per entry) |
| `0x1000DE40` | `NokComponentArray_CopyRange` | Copy range of component entries |
| `0x1000DE80` | `NokComponentArray_FillRange` | Fill range with template entry |
| `0x1000DEC0` | `NokComponentArray_CopySingle` | Copy one entry (0x210 bytes) |
| `0x1000DF20` | `NokMediaManager_Release` | Release with ref-count decrement |

The WV2WAV system is called from `DXSound::Create` (0x10007870) during sound initialization. It attempts to load a "wv2wav" audio codec from the CD install source (read from `HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\InstallSource`), looking for `"TD5 Track (Wv2)"` under that path. The "Nok" / "AMKRamkr" verification is a signature check on loaded DLLs, possibly related to the game's copy protection or the audio codec's DRM.

## FMV / Movie Playback System (In EXE, Not M2DXFX.dll)

The TGQ movie player is entirely within `TD5_d3d.exe` (NOT in M2DXFX.dll):

| EXE Address | Function | Purpose |
|---|---|---|
| `0x0043C3C0` | `RequestIntroMovieShutdown` | Callback: stop playback, release surfaces |
| `0x0043C780` | `PlayIntroMovie` | Top-level: setup DD surfaces, open "Movie\intro.tgq", pump messages |
| `0x00451990` | `OpenMultimediaStream` | Alloc 0x668-byte stream context, open WAV, init video decoder |
| `0x00451A10` | `OpenWavStream` | Parse TGQ container, extract audio |
| `0x00452910` | `LaunchStreamPlaybackThread` | Create playback thread or run synchronous |
| `0x00452990` | `StreamThreadEntryPoint` | Main decode loop |
| `0x00452E20` | `OpenAndStartMediaPlayback` | Wrapper: open + launch |
| `0x00452E60` | `SetStreamVolume` | Set volume on stream context |
| `0x00452E80` | `IsStreamPlaying` | Check if stream is still active |
| `0x00452EC0` | `StopStreamPlayback` | Request stop, wait for thread exit |
| `0x004534E0` | `BuildColorConversionLUTs` | Build YUV-to-RGB lookup tables |
| `0x004539E0` | `InitializeVideoDecoder` | Initialize IDCT/DCT state for TGQ decode |
| `0x00456110` | `InitStreamDirectSound` | Create DS buffer for audio portion |

The movie system plays EA TGQ format files (IDCT-based video + IMA ADPCM audio). See `re/analysis/ea-tgq-multimedia-engine.md` for format details.

## Key Findings Summary

1. **M2DXFX.dll is NOT an FMV DLL** -- it is a stripped M2DX engine variant without Direct3D support.
2. **The EXE never loads M2DXFX.dll** -- it imports only from M2DX.dll.
3. **M2DXFX.dll was likely used by the installer** or as a fallback for non-3D systems.
4. **NoMovie/FixMovie flags** are parsed by both DLLs' command-line handlers but only read by the EXE through the shared `DX::app` struct.
5. **The TGQ movie player** is compiled into the EXE at 0x451990-0x456110.
6. **WV2WAV codec** is a CD audio streaming system with DRM verification ("AMKRamkr" / "GetNokComponent").
7. **The previous "3dfx-specific" hypothesis was incorrect** -- M2DXFX.dll has no Glide/3dfx code; it simply omits DXD3D.
