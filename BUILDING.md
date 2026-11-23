# Building bms-dlss from source

bms-dlss consists of two binaries:

- `d3d9.dll` – 32-bit proxy loaded by Black Mesa in place of the game's DXVK `d3d9.dll` (the original is kept as
  `d3d9_dxvk.dll` and all calls are forwarded to it). It finds the scene, depth, camera and motion vectors, draws the
  in-game menu and limits the frame rate.
- `bms_dlss_helper.exe` – 64-bit helper process (Vulkan) that runs NVIDIA DLSS / DLSS Frame Generation (NGX) and
  AMD FSR / FSR Frame Generation on images shared with the proxy.

## Requirements

- Windows 10/11, PowerShell 5.1.
- **Visual Studio 2022** with "Desktop development with C++" (MSVC x86 and x64, Windows SDK). The build scripts find
  it with `vswhere`.

## Third-party SDKs (not included – download them into `third_party\`)

The NVIDIA DLSS SDK license does not allow redistributing the SDK in source form, so no SDK is part of this
repository. Place the following folders next to `src\`:

| folder | what | where to get it |
|---|---|---|
| `third_party\DLSS` | NVIDIA DLSS SDK 310.x, the release uses the 310.9.16 DLLs (headers `include\`, `lib\Windows_x86_64\x64\nvsdk_ngx_s.lib`, release DLLs in `lib\Windows_x86_64\rel\`) | https://github.com/NVIDIA/DLSS |
| `third_party\FidelityFX-SDK` | AMD FSR SDK 2.4.0 (`Kits\FidelityFX` – ffx API headers and the signed DLLs) | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK |
| `third_party\Vulkan-Headers` | Vulkan headers (tested with 1.4.368) | https://github.com/KhronosGroup/Vulkan-Headers |
| `third_party\glslang\bin\glslang.exe` | glslang shader compiler (tested with 16.x) | https://github.com/KhronosGroup/glslang/releases |
| `third_party\dxvk-interop` | `d3d9_interop.h`, `d3d9_interfaces.h` and `LICENSE` from DXVK `src\d3d9` (same version as the game, 3.1.1) | https://github.com/doitsujin/dxvk |

Only needed to regenerate the frame interpolation shaders (pre-generated headers are in `src\helper\afg\gen`):
`third_party\FidelityFX-SDK-v1.1.4` (AMD FidelityFX SDK 1.1.4) and `third_party\FidelityFX_SC-v1.1.4`
(its shader compiler), used by `src\helper\afg\gen_shaders.ps1`.

## Build

```powershell
# 1. frame interpolation library (fork of AMD FSR 3.1.4, MIT) -> build\x64\afg.lib
powershell -ExecutionPolicy Bypass -File src\helper\afg\build_afg.ps1

# 2. proxy + helper + compute shaders -> build\d3d9.dll, build\bms_dlss_helper.exe
#    (also copies nvngx_dlss.dll, nvngx_dlssg.dll, the AMD DLLs and the x64 VC++ runtime into build\)
powershell -ExecutionPolicy Bypass -File build.ps1
```

Both builds compile with `/W3`; the mod's own code builds without warnings.

## Install for testing

Close the game, then:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1 -GameDir "<path to Black Mesa>"
```

Start the game with the Steam launch option "Play DXVK". `uninstall.ps1 -GameDir "<path>"` restores the original
DXVK. Settings are in `bin\thirdparty\dxvk-windows-x86\bms_dlss.ini` (the template `bms_dlss.ini` documents the player-facing keys),
logs are `bms_dlss.log` (proxy) and `bms_dlss_helper.log` (helper) in the same folder.

## Release package

```powershell
powershell -ExecutionPolicy Bypass -File tools\release\pack.ps1 -Release -Readme tools\release\README-release.md
```

creates `dist\bms-dlss-<version>.zip` with `SHA256SUMS.txt`. The version is taken from `src\res\version.h`.

The build is not bit-for-bit reproducible (PE timestamps). The third-party DLLs in the release are the unmodified
signed files from the SDKs - compare their SHA256 with `SHA256SUMS.txt` in the release zip.

## Licenses

The mod's code is MIT licensed (`LICENSE`). NVIDIA DLSS, AMD FidelityFX / FSR, DXVK interop headers and the Vulkan
headers are under their own licenses – see `THIRD_PARTY_NOTICES.md`.
