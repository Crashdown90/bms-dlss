# Third-party notices

bms-dlss is an unofficial, free mod for Black Mesa. It is not affiliated with, sponsored by or endorsed by
Crowbar Collective, Valve, NVIDIA, AMD or Microsoft. The bms-dlss source code is released under the MIT License (see
`LICENSE`). The third-party components listed below are **not** covered by the MIT License; each one remains under
its own license, reproduced or referenced here as that license requires.

| Component | Shipped in the release as | License | Upstream |
| --- | --- | --- | --- |
| NVIDIA DLSS SDK / NGX SDK (DLSS Super Resolution, DLSS Frame Generation) | `nvngx_dlss.dll`, `nvngx_dlssg.dll` (310.9.16, signed by NVIDIA); NGX SDK static library and helper headers compiled into `bms_dlss_helper.exe` | NVIDIA RTX SDKs License (v. March 14, 2024), section 1 below | https://github.com/NVIDIA/DLSS |
| AMD FSR SDK 2.4.0 ("Redstone") runtime | `amd_fidelityfx_loader_vk.dll` (1.0.2), `amd_fidelityfx_upscaler_vk.dll` (4.1.2), `amd_fidelityfx_framegeneration_vk.dll` (4.0.2), signed by AMD | AMD binary redistribution license, section 2 below | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (tag v2.4.0) |
| AMD FidelityFX API headers (FSR SDK 2.4.0) | compiled into `bms_dlss_helper.exe` | MIT, section 3 below | same as above |
| AMD FidelityFX FSR 1 RCAS (sharpening) | GLSL port in `src/helper/sharpen.comp`, compiled into `bms_dlss_helper.exe` | MIT, section 3 below | https://github.com/GPUOpen-Effects/FidelityFX-FSR |
| AMD FidelityFX SDK 1.1.4 (FSR 3.1.4 frame interpolation, optical flow, Vulkan backend; modified) | compiled into `bms_dlss_helper.exe` (adaptive frame generation); source in `src/helper/afg/ffx/` | MIT, `src/helper/afg/ffx/LICENSE`, section 3 below | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (tag v1.1.4) |
| Microsoft Visual C++ Redistributable (x64 runtime, Visual Studio 2022) | `vcrt/msvcp140.dll`, `vcrt/vcruntime140.dll`, `vcrt/vcruntime140_1.dll` (14.44, signed by Microsoft, unmodified); used only by the AMD libraries when the system copy is missing or older than 14.44 | Microsoft Software License Terms for Visual Studio 2022, "Distributable Code", section 6 below | https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist |
| DXVK (D3D9 interop interface declarations) | `src/dxvk_interop.h` (re-declared interfaces of DXVK 3.1.1 `src/d3d9/d3d9_interfaces.h`), compiled into `d3d9.dll` | zlib/libpng, section 4 below | https://github.com/doitsujin/dxvk |
| Vulkan-Headers (Khronos) | compiled into `d3d9.dll` and `bms_dlss_helper.exe` (`VK_HEADER_VERSION` 368) | Apache-2.0 OR MIT, section 5 below | https://github.com/KhronosGroup/Vulkan-Headers |

bms-dlss does not ship DXVK itself: the proxy `d3d9.dll` forwards all calls to the DXVK build that comes with
Black Mesa (`bin\thirdparty\dxvk-windows-x86`, renamed to `d3d9_dxvk.dll` by the installer). DXVK is
Copyright (c) 2017 Philip Rebohle, Copyright (c) 2019 Joshua Ashton and contributors, zlib/libpng license.

Build tools that are not distributed (Microsoft Visual C++, Windows SDK `fxc.exe` / `rc.exe`, Khronos glslang)
have no notice requirements for the produced binaries.

Trademarks: NVIDIA, GeForce, RTX and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation.
AMD, Radeon, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc. Vulkan is a registered trademark of
the Khronos Group Inc. Black Mesa is a trademark of Crowbar Collective. Half-Life is a trademark of Valve
Corporation. All other trademarks are the property of their respective owners and are used here only to describe
compatibility.

---

## 1. NVIDIA DLSS SDK / NGX SDK

This software contains source code provided by NVIDIA Corporation.

bms-dlss uses NVIDIA DLSS (Deep Learning Super Sampling) and NVIDIA DLSS Frame Generation through the NVIDIA NGX
SDK. The files `nvngx_dlss.dll` and `nvngx_dlssg.dll` and the NGX SDK code compiled into `bms_dlss_helper.exe` are
Copyright (c) NVIDIA Corporation. All rights reserved. They are distributed under the NVIDIA RTX SDKs License
(v. March 14, 2024) as part of this application and not as a stand-alone product. They are not licensed under the
MIT License of bms-dlss.

By using these NVIDIA components you agree to the following terms, which are at least as protective of NVIDIA as
the NVIDIA RTX SDKs License:

- The NVIDIA components may be used only together with bms-dlss and only on systems with NVIDIA GPUs. They may not
  be copied, sold, rented, sublicensed, transferred or distributed separately from bms-dlss.
- You may not reverse engineer, decompile or disassemble the NVIDIA components, or remove copyright or other
  proprietary notices from them.
- You may not modify the NVIDIA components or create derivative works of them, and you may not bypass, disable or
  circumvent any technical limitation, encryption, security, digital rights management or authentication mechanism
  in them.
- You may not use the NVIDIA components in any system or application where their use or failure could reasonably
  be expected to threaten or result in personal injury, death or catastrophic loss.
- NVIDIA and its suppliers keep all rights, title and interest in the NVIDIA components. NVIDIA may update the DLSS
  runtime over the air (for example through the NVIDIA App or the driver).
- THE NVIDIA COMPONENTS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND. TO THE MAXIMUM EXTENT PERMITTED BY
  APPLICABLE LAW, NVIDIA, ITS AFFILIATES AND THE AUTHORS OF bms-dlss SHALL NOT BE LIABLE FOR ANY DAMAGES ARISING
  OUT OF THEIR USE.
- These components are subject to United States export laws and regulations.
- bms-dlss is not sponsored or endorsed by NVIDIA.

Full license text: `https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt` (in the source tree of the developer:
`third_party/DLSS/LICENSE.txt`).

## 2. AMD FSR SDK 2.4.0 runtime libraries

Applies to `amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll` and
`amd_fidelityfx_framegeneration_vk.dll` (unmodified, signed by Advanced Micro Devices, Inc.). Source:
`Kits/FidelityFX/signedbin/` of the AMD FidelityFX SDK, tag v2.4.0; license file `Kits/FidelityFX/docs/license.md`.

```
Copyright (C) Advanced Micro Devices, Inc. All rights reserved.

REDISTRIBUTION: Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to install, reproduce, copy and distribute copies of the Software,
in binary form only, and to permit persons to whom the Software is provided to do the same, provided that the
following conditions are met:

No reverse engineering, decompilation, or disassembly of this Software is permitted.

Redistributions must reproduce the above copyright notice, this permission notice, and the following disclaimers
and notices in the Software documentation and/or other materials provided with the Software.

DISCLAIMER: THE USE OF THE SOFTWARE IS AT YOUR SOLE RISK. THE SOFTWARE IS PROVIDED "AS IS" AND WITHOUT WARRANTY OF
ANY KIND AND COPYRIGHT HOLDER AND ITS LICENSORS EXPRESSLY DISCLAIM ALL WARRANTIES, EXPRESS AND IMPLIED, INCLUDING,
BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NON-INFRINGEMENT. COPYRIGHT HOLDER AND ITS LICENSORS DO NOT WARRANT THAT THE SOFTWARE WILL MEET YOUR REQUIREMENTS,
OR THAT THE OPERATION OF THE SOFTWARE WILL BE UNINTERRUPTED OR ERROR-FREE. THE ENTIRE RISK ASSOCIATED WITH THE USE
OF THE SOFTWARE IS ASSUMED BY YOU. FURTHERMORE, COPYRIGHT HOLDER AND ITS LICENSORS DO NOT WARRANT OR MAKE ANY
REPRESENTATIONS REGARDING THE USE OR THE RESULTS OF THE USE OF THE SOFTWARE IN TERMS OF ITS CORRECTNESS, ACCURACY,
RELIABILITY, CURRENTNESS, OR OTHERWISE.

DISCLAIMER: UNDER NO CIRCUMSTANCES INCLUDING NEGLIGENCE, SHALL COPYRIGHT HOLDER AND ITS LICENSORS OR ITS
DIRECTORS, OFFICERS, EMPLOYEES OR AGENTS ("AUTHORIZED REPRESENTATIVES") BE LIABLE FOR ANY INCIDENTAL, INDIRECT,
SPECIAL OR CONSEQUENTIAL DAMAGES (INCLUDING DAMAGES FOR LOSS OF BUSINESS PROFITS, BUSINESS INTERRUPTION, LOSS OF
BUSINESS INFORMATION, AND THE LIKE) ARISING OUT OF THE USE, MISUSE OR INABILITY TO USE THE SOFTWARE, BREACH OR
DEFAULT, INCLUDING THOSE ARISING FROM INFRINGEMENT OR ALLEGED INFRINGEMENT OF ANY PATENT, TRADEMARK, COPYRIGHT OR
OTHER INTELLECTUAL PROPERTY RIGHT EVEN IF COPYRIGHT HOLDER AND ITS AUTHORIZED REPRESENTATIVES HAS BEEN ADVISED OF
THE POSSIBILITY OF SUCH DAMAGES. IN NO EVENT SHALL COPYRIGHT HOLDER OR ITS AUTHORIZED REPRESENTATIVES TOTAL
LIABILITY FOR ALL DAMAGES, LOSSES, AND CAUSES OF ACTION (WHETHER IN CONTRACT, TORT (INCLUDING NEGLIGENCE) OR
OTHERWISE) EXCEED THE AMOUNT OF US$10.
```

The same `license.md` also lists these DLLs among the files available under the MIT License (section 3). The
stricter terms above are followed in either case.

## 3. AMD FidelityFX (MIT)

Applies to the FidelityFX API headers compiled into `bms_dlss_helper.exe`, the GLSL port of FSR 1 RCAS in
`src/helper/sharpen.comp`, and the modified FidelityFX SDK 1.1.4 sources in `src/helper/afg/ffx/`, compiled into
`bms_dlss_helper.exe` for adaptive frame generation (changes are
marked "upraveno (bms-dlss)" and listed in `src/helper/afg/ffx/LICENSE`).

```
Copyright (C) 2021-2026 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of
the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 4. DXVK (zlib/libpng)

`src/dxvk_interop.h` re-declares the D3D9 interop interfaces of DXVK 3.1.1 (`src/d3d9/d3d9_interfaces.h`,
`ID3D9VkInteropDevice`, `ID3D9VkInteropTexture`). It is an altered version of that header and is not the original
software.

```
Copyright (c) 2017 Philip Rebohle
Copyright (c) 2019 Joshua Ashton
Copyright (c) 2019 Robin Kertels
Copyright (c) 2023 Jeffrey Ellison

                        zlib/libpng license

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

- The origin of this software must not be misrepresented; you must not
  claim that you wrote the original software. If you use this software
  in a product, an acknowledgment in the product documentation would be
  appreciated but is not required.

- Altered source versions must be plainly marked as such, and must not
  be misrepresented as being the original software.

- This notice may not be removed or altered from any source distribution.
```

## 5. Vulkan-Headers

Copyright 2015-2026 The Khronos Group Inc. Used under the MIT License (the headers are dual-licensed
`Apache-2.0 OR MIT`, SPDX identifier in each file).

```
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of
the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 6. Microsoft Visual C++ Redistributable (x64 runtime)

`vcrt/msvcp140.dll`, `vcrt/vcruntime140.dll` and `vcrt/vcruntime140_1.dll` are copied unmodified from the
redistributable folder of Visual Studio 2022 (`VC/Redist/MSVC/<version>/x64/Microsoft.VC143.CRT`). They are
"Distributable Code" listed in the Visual Studio 2022 REDIST list
(https://learn.microsoft.com/visualstudio/releases/2022/redistribution) and are distributed under the Microsoft
Software License Terms for Visual Studio 2022. Copyright (c) Microsoft Corporation. All rights reserved.

These files are the property of Microsoft Corporation and are **not** covered by the MIT License of bms-dlss. They
may be used only together with bms-dlss; you may not modify, reverse engineer or redistribute them separately.
Microsoft provides them "as is" and gives no warranties; to the extent permitted by law Microsoft is not liable for
any damages arising from their use. Microsoft, Visual Studio and Visual C++ are trademarks of the Microsoft group of
companies.

The helper prefers the system runtime installed by the official Microsoft Visual C++ Redistributable x64
(`vc_redist.x64.exe`, serviced by Windows Update) and loads these copies only when the system runtime is missing or
older than version 14.44. You may delete the `vcrt` folder if the official redistributable is installed.
