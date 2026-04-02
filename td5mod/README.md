# TD5 Mod Framework

ASI-based mod framework for Test Drive 5 (1999). Hooks game functions via MinHook
to add features without modifying the original EXE.

## Prerequisites

You need **one** of these:

### Option A: Visual Studio 2022 (recommended)
1. Install [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)
2. Select "Desktop development with C++" workload
3. Make sure to check "C++ CMake tools" component

### Option B: MSYS2 + MinGW (lighter)
1. Install [MSYS2](https://www.msys2.org/)
2. In MSYS2 terminal: `pacman -S mingw-w64-i686-gcc mingw-w64-i686-cmake`
3. Add `C:\msys64\mingw32\bin` to PATH

### Also needed
- [CMake](https://cmake.org/download/) 3.15+ (included with VS2022 Build Tools or MSYS2)
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) — download the x86 `dinput.dll` version
- [Git](https://git-scm.com/) (for MinHook auto-download)

## Build

### Visual Studio 2022 (Developer Command Prompt x86)
```cmd
cd td5mod
cmake -B build -A Win32
cmake --build build --config Release
```

### MinGW
```cmd
cd td5mod
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

Output: `build/scripts/td5_mod.asi`

## Install

1. Copy `dinput.dll` (Ultimate ASI Loader, x86) to the TD5 game directory
2. Create `scripts/` folder in the game directory
3. Copy `build/scripts/td5_mod.asi` to `scripts/`
4. Copy `dist/scripts/td5_mod.ini` to `scripts/`
5. Launch the game normally

## Configuration

Edit `scripts/td5_mod.ini`:

```ini
[Features]
DamageWobble=1    ; Restore cut collision wobble (safe, recommended)
SnowRendering=1   ; Restore cut snow on level003
WidescreenFix=0   ; Enable for non-4:3 resolutions
DebugOverlay=0    ; Show FPS/speed/gear/span during races
```

## Adding New Features

1. Define the hook target in `td5_sdk.h` (address + typedef)
2. Write your hook function in `td5_mod.c`
3. Register the hook in `InstallHooks()`
4. Add an INI toggle in `LoadConfig()`
5. Build and test

### Hook template
```c
static fn_TargetFunc Original_TargetFunc = NULL;

static void __cdecl Hook_TargetFunc(void) {
    /* Your pre-call logic */
    Original_TargetFunc();  /* Call original via trampoline */
    /* Your post-call logic */
}

/* In InstallHooks(): */
MH_CreateHook((LPVOID)0xADDRESS, &Hook_TargetFunc, (LPVOID*)&Original_TargetFunc);
MH_EnableHook((LPVOID)0xADDRESS);
```

### Calling game functions
```c
/* Call any game function directly via its known VA */
TD5_QueueRaceHudFormattedText(100, 50, "Hello from mod!");
```

### Reading game state
```c
int state = g_gameState;           /* 0=intro, 1=menu, 2=race */
float fps = 30.0f / g_normalizedFrameDt;
int speed = *(int32_t*)(TD5_ACTOR(0) + ACTOR_OFF_FORWARD_SPEED);
```

## Debugging

- Messages go to `OutputDebugStringA` — view with [DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview) or x64dbg
- Attach x64dbg to `TD5_d3d.exe` after launch to set breakpoints in your hooks
- Your DLL has symbols if built in Debug mode (`cmake --build build --config Debug`)

## Architecture

```
TD5_d3d.exe (UNMODIFIED)
  ├── ddraw.dll (dgVoodoo2)
  └── dinput.dll (Ultimate ASI Loader)
       └── scripts/td5_mod.asi (this project)
            ├── MinHook trampolines
            ├── td5_sdk.h (game globals + function pointers)
            └── Feature hooks (damage wobble, snow, widescreen, debug)
```

## Credits

- Reverse engineering: Complete Ghidra analysis of all 1,761 functions across 4 binaries
- Game: Test Drive 5 by Pitbull Syndicate / Infogrames (1999)
