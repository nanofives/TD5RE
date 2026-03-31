# True Widescreen Status

Status: deferred as a longer-term binary patch project.

Stable runtime state:
- plain dgVoodoo2 wrapper restored in `ddraw.dll`
- no active viewport override
- no active projection override
- no active mode coercion

What was proven:
- wrapper-level mode coercion can break rendering
- viewport override alone does not produce visible widescreen
- identity projection override alone does not produce visible widescreen
- TD5 still configures a legacy `640x480` viewport in the Direct3D3 path
- `M2DX.dll` also knows about the higher internal render size, but the visible 4:3 camera/frustum is controlled deeper in engine code

Current recommendation:
- keep the Windows 11 compatibility baseline as the production setup
- treat true widescreen as a separate reverse-engineering milestone
- resume only when there is time to map and patch the engine camera/FOV code in `M2DX.dll`
