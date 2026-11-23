# Prelozi 32bitovou proxy d3d9.dll a 64bitovy helper pomoci MSVC (Visual Studio 2022).
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio s C++ nastroji nenalezeno." }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$env:PATH = "$(Split-Path $vswhere);$env:PATH"   # vcvarsall si vswhere hleda v PATH
New-Item -ItemType Directory -Force (Join-Path $root 'build') | Out-Null

# Vulkan hlavicky do kratke cesty (cl.exe neumi cesty delsi nez 260 znaku)
$vkInc = Join-Path $env:TEMP 'bms-dlss-vk'
if (-not (Test-Path "$vkInc\vulkan\vulkan.h")) {
    New-Item -ItemType Directory -Force $vkInc | Out-Null
    Copy-Item "\\?\$(Join-Path $root 'third_party\Vulkan-Headers\include')\*" $vkInc -Recurse -Force
}

# Generovane hlavicky (bytecode shaderu proxy i helperu)
$gen = Join-Path $root 'build\gen'
New-Item -ItemType Directory -Force $gen | Out-Null

# Shadery proxy pro MV pohyblivych objektu (D3D9, vs_3_0 / ps_3_0): fxc z Windows SDK -> build\gen\objmv_<jmeno>.h
# s polem kObjMv<Jmeno> (vklada je src\objmv.inl)
$fxc = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x86\fxc.exe"
if (-not (Test-Path $fxc)) {
    # jina verze SDK: nejnovejsi x86 fxc.exe ve Windows Kits
    $found = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x86\fxc.exe" -ErrorAction SilentlyContinue |
             Sort-Object FullName | Select-Object -Last 1
    if (-not $found) { throw "fxc.exe (Windows SDK) nenalezen: $fxc" }
    $fxc = $found.FullName
}
# Polozka: profil, vstupni funkce, jmeno pole, vystupni hlavicka
foreach ($sh in @(@('vs_3_0', 'VSBrush', 'kObjMvVsBrush', 'objmv_vs_brush'),
                  @('vs_3_0', 'VSRigid', 'kObjMvVsRigid', 'objmv_vs_rigid'),
                  @('vs_3_0', 'VSSkinned', 'kObjMvVsSkin', 'objmv_vs_skin'),
                  @('ps_3_0', 'PSMain', 'kObjMvPs', 'objmv_ps'))) {
    $src = "$root\src\shaders\objmv.hlsl"
    $hdr = "$gen\$($sh[3]).h"
    # /WX: varovani je chyba. fxc pise i hlaseni o uspechu; pres cmd 2>&1 a vypsat jen pri chybe (jako u glslang).
    $out = cmd /c "`"$fxc`" /nologo /O3 /WX /T $($sh[0]) /E $($sh[1]) /Vn $($sh[2]) /Fh `"$hdr`" `"$src`" 2>&1"
    if ($LASTEXITCODE -ne 0) { $out | Write-Host; throw "Preklad shaderu $src ($($sh[1])) selhal (kod $LASTEXITCODE)." }
    Write-Host "OK: $hdr"
}

# /utf-8: texty menu (src/menu_text.inl) jsou v UTF-8; gdi32: atlas pisma menu (src/menu.inl)
# VERSIONINFO (src\res\d3d9.rc, verze v src\res\version.h)
$cmd = "call `"$vcvars`" x86 >nul && cd /d `"$root`" && " +
       "rc /nologo /fo build\d3d9.res src\res\d3d9.rc && " +
       "cl /nologo /O2 /Zi /MT /EHsc /std:c++17 /W3 /utf-8 /LD /Fdbuild\d3d9_cl.pdb /I`"$vkInc`" /I`"$gen`" " +
       "src\proxy.cpp src\interop.cpp src\present.cpp build\d3d9.res /Fobuild\ /Febuild\d3d9.dll " +
       "/link /DEBUG /OPT:REF /OPT:ICF /PDB:build\d3d9.pdb /PDBALTPATH:d3d9.pdb /DEF:src\d3d9.def /MACHINE:X86 user32.lib gdi32.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Preklad selhal (kod $LASTEXITCODE)." }
Write-Host "OK: $(Join-Path $root 'build\d3d9.dll')"

# Compute shadery helperu -> SPIR-V jako C hlavicky (build\gen\<jmeno>_comp.h, pole k<Jmeno>CompSpv)
$glslang = Join-Path $root 'third_party\glslang\bin\glslang.exe'
if (-not (Test-Path $glslang)) { throw "Chybi $glslang." }
# Polozka: zdroj, jmeno pole, vystupni hlavicka, makra (alpha.comp ve variante HDR_IN pro linearni vstup DLSS;
# downsample.comp: HDR_IN = zdroj RGBA16F, MAX_SRC=26 = mensi dlazdice pro S <= 2.18, rychlejsi)
foreach ($sh in @(@('mv', 'kMvCompSpv', 'mv', ''), @('alpha', 'kAlphaCompSpv', 'alpha', ''),
                  @('alpha', 'kAlphaHdrCompSpv', 'alpha_hdr', '-DHDR_IN'),
                  @('linearize', 'kLinearizeCompSpv', 'linearize', ''),
                  @('sharpen', 'kSharpenCompSpv', 'sharpen', ''),
                  @('fgsharp', 'kFgSharpCompSpv', 'fgsharp', ''),
                  @('uialpha', 'kUiAlphaCompSpv', 'uialpha', ''),
                  @('depthmin', 'kDepthMinCompSpv', 'depthmin', ''),
                  @('reactive', 'kReactiveCompSpv', 'reactive', ''),
                  @('downsample', 'kDownsampleCompSpv', 'downsample', ''),
                  @('downsample', 'kDownsampleHdrCompSpv', 'downsample_hdr', '-DHDR_IN'),
                  @('downsample', 'kDownsampleSmallCompSpv', 'downsample_s', '-DMAX_SRC=26'),
                  @('downsample', 'kDownsampleSmallHdrCompSpv', 'downsample_hdr_s', '-DHDR_IN -DMAX_SRC=26'))) {
    $src = "$root\src\helper\$($sh[0]).comp"
    $hdr = "$gen\$($sh[2])_comp.h"
    # (glslang pise jmeno souboru na stderr; pres cmd 2>&1, at to PowerShell nebere jako chybu)
    $out = cmd /c "`"$glslang`" -V --target-env vulkan1.1 $($sh[3]) --vn $($sh[1]) -o `"$hdr`" `"$src`" 2>&1"
    if ($LASTEXITCODE -ne 0) { $out | Write-Host; throw "Preklad shaderu $src selhal (kod $LASTEXITCODE)." }
    Write-Host "OK: $hdr"
}

# 64bitovy pomocny proces (NGX SDK staticky: nvsdk_ngx_s.lib pro /MT). FSR: jen hlavicky ffx API z FidelityFX SDK 2.4
# (Kits\FidelityFX), podepsane DLL se nacitaji za behu (LoadLibrary); wintrust + crypt32 = kontrola podpisu AMD.
$dlss = Join-Path $root 'third_party\DLSS'
$ffx = Join-Path $root 'third_party\FidelityFX-SDK\Kits\FidelityFX'
if (-not (Test-Path "$ffx\upscalers\include\ffx_upscale.h")) { throw "Chybi FidelityFX SDK 2.4: $ffx" }
New-Item -ItemType Directory -Force (Join-Path $root 'build\x64') | Out-Null
# Adaptivni FSR FG (fork FSR 3.1.4 s casem t, MIT) -> build\x64\afg.lib (preklada se jen pri zmene src\helper\afg)
& (Join-Path $root 'src\helper\afg\build_afg.ps1')
if (-not (Test-Path (Join-Path $root 'build\x64\afg.lib'))) { throw "Chybi build\x64\afg.lib (src\helper\afg\build_afg.ps1)." }
# /DEPENDENTLOADFLAG:0x800 = staticke importy helperu (version, wintrust, crypt32 ...) jen ze System32
$cmd = "call `"$vcvars`" x64 >nul && cd /d `"$root`" && " +
       "rc /nologo /fo build\x64\helper.res src\res\helper.rc && " +
       "cl /nologo /O2 /Zi /MT /EHsc /std:c++17 /W3 /Fdbuild\x64\helper_cl.pdb /I`"$vkInc`" /I`"$dlss\include`" /I`"$gen`" /I`"$ffx`" " +
       "src\helper\helper.cpp src\helper\present.cpp build\x64\helper.res /Fobuild\x64\ /Febuild\bms_dlss_helper.exe " +
       "/link /DEBUG /OPT:REF /OPT:ICF /PDB:build\bms_dlss_helper.pdb /PDBALTPATH:bms_dlss_helper.pdb /SUBSYSTEM:WINDOWS /MACHINE:X64 /DEPENDENTLOADFLAG:0x800 /LIBPATH:`"$dlss\lib\Windows_x86_64\x64`" " +
       "nvsdk_ngx_s.lib user32.lib advapi32.lib wintrust.lib crypt32.lib version.lib build\x64\afg.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Preklad helperu selhal (kod $LASTEXITCODE)." }
Write-Host "OK: $(Join-Path $root 'build\bms_dlss_helper.exe')"

# Runtime DLSS (release, bez ladiciho overlaye) vedle helperu
$ngxDll = Join-Path $dlss 'lib\Windows_x86_64\rel\nvngx_dlss.dll'
$ngxDst = Join-Path $root 'build\nvngx_dlss.dll'
if (-not (Test-Path $ngxDll)) { throw "Chybi $ngxDll." }
if (-not (Test-Path $ngxDst) -or (Get-Item $ngxDll).Length -ne (Get-Item $ngxDst).Length -or
    (Get-Item $ngxDll).LastWriteTimeUtc -ne (Get-Item $ngxDst).LastWriteTimeUtc) {
    Copy-Item $ngxDll $ngxDst -Force
}
Write-Host "OK: $ngxDst"

# Runtime DLSS Frame Generation (DLSS-G, [fg] Mode=dlssg) vedle helperu: jen RELEASE knihovna z SDK (lib/.../rel).
# Vyvojova (dev, s ladicim overlayem a klavesami) se nesmi dostat do buildu ani do hry - proto kontrola proti ni.
$ngxgDll = Join-Path $dlss 'lib\Windows_x86_64\rel\nvngx_dlssg.dll'
$ngxgDev = Join-Path $dlss 'lib\Windows_x86_64\dev\nvngx_dlssg.dll'
$ngxgDst = Join-Path $root 'build\nvngx_dlssg.dll'
if (-not (Test-Path $ngxgDll)) { throw "Chybi $ngxgDll." }
if ((Get-AuthenticodeSignature $ngxgDll).Status -ne 'Valid') { throw "$ngxgDll nema platny podpis." }
if ((Test-Path $ngxgDev) -and (Get-FileHash $ngxgDll).Hash -eq (Get-FileHash $ngxgDev).Hash) {
    throw "$ngxgDll je shodna s vyvojovou (dev) knihovnou - do buildu smi jen release."
}
if (-not (Test-Path $ngxgDst) -or (Get-FileHash $ngxgDll).Hash -ne (Get-FileHash $ngxgDst).Hash) {
    Copy-Item $ngxgDll $ngxgDst -Force
}
Write-Host "OK: $ngxgDst"

# Runtime AMD FSR (ffx API, podepsane AMD) vedle helperu: loader + upscaler (FSR 4.x pro AMD RDNA, 3.1.x pro ostatni)
# + frame generation ([fg] Mode=fsr2x; FSR FG 3.1.x na vsech kartach). Helper pred nactenim overuje podpis AMD
# ([upscale] FsrSignatureCheck).
foreach ($name in @('amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll',
                    'amd_fidelityfx_framegeneration_vk.dll')) {
    $srcDll = Join-Path $ffx "signedbin\$name"
    $dstDll = Join-Path $root "build\$name"
    if (-not (Test-Path $srcDll)) { throw "Chybi $srcDll." }
    if ((Get-AuthenticodeSignature $srcDll).Status -ne 'Valid') { throw "$srcDll nema platny podpis." }
    if (-not (Test-Path $dstDll) -or (Get-Item $srcDll).Length -ne (Get-Item $dstDll).Length -or
        (Get-Item $srcDll).LastWriteTimeUtc -ne (Get-Item $dstDll).LastWriteTimeUtc) {
        Copy-Item $srcDll $dstDll -Force
    }
    Write-Host "OK: $dstDll"
}

# x64 Visual C++ runtime (msvcp140, vcruntime140, vcruntime140_1) pro DLL AMD do build\vcrt: redistribuovatelne
# soubory Visual Studia (VC\Redist\MSVC\<verze>\x64\Microsoft.VC14x.CRT, licence VS dovoluje je sirit s programem
# beze zmeny). Helper je nacte jen kdyz v System32 runtime chybi nebo je starsi nez 14.44 (src\helper\helper.cpp,
# CheckVcRuntime); jinak se pouzije systemovy (aktualizuje ho Windows Update / vc_redist).
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT') -Directory -ErrorAction SilentlyContinue |
       Sort-Object { [version](Split-Path (Split-Path (Split-Path $_.FullName -Parent) -Parent) -Leaf) } |
       Select-Object -Last 1
if (-not $crt) { throw "Redistribuovatelny runtime x64 nenalezen (VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT)." }
$vcrtDst = Join-Path $root 'build\vcrt'
New-Item -ItemType Directory -Force $vcrtDst | Out-Null
foreach ($name in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    $srcDll = Join-Path $crt.FullName $name
    if (-not (Test-Path $srcDll)) { throw "Chybi $srcDll." }
    $sig = Get-AuthenticodeSignature $srcDll
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Microsoft Corporation') { throw "$srcDll nema platny podpis Microsoftu." }
    $dstDll = Join-Path $vcrtDst $name
    if (-not (Test-Path $dstDll) -or (Get-FileHash $srcDll).Hash -ne (Get-FileHash $dstDll).Hash) { Copy-Item $srcDll $dstDll -Force }
}
Write-Host "OK: $vcrtDst ($((Get-Item (Join-Path $vcrtDst 'msvcp140.dll')).VersionInfo.FileVersion) z $($crt.FullName))"
