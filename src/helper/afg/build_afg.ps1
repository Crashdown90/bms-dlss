# Prelozi AFG (fork FSR 3.1.4 frame interpolation + optical flow, src\helper\afg\ffx, a obalku afg.cpp) do statické
# knihovny build\x64\afg.lib pro 64bitovy helper. Toolchain jako build.ps1: VS 2022 x64, /O2 /MT /EHsc /std:c++17,
# Vulkan hlavicky z third_party\Vulkan-Headers (kopie v %TEMP%\bms-dlss-vk kvuli delce cest), VK_NO_PROTOTYPES.
# Shadery se nepřekládají - hlavicky permutaci jsou v src\helper\afg\gen (pregenerovani: gen_shaders.ps1).
# Kod forku AMD i obalka afg.cpp: /W3, 0 varovani.
# Pri nezmenenych zdrojacich se nic neprekládá (knihovna je novejsi nez vsechny soubory v src\helper\afg).
#   .\build_afg.ps1                    -> build\x64\afg.lib
#   .\build_afg.ps1 -Force             vzdy prelozit
#   .\build_afg.ps1 -Reference         test: nemodifikovane shadery 1.1.4 (tools\spike_afg\gen_ref) + /DAFG_REFERENCE
#                                      -> tools\spike_afg\build\afg_ref.lib
param([switch]$Reference, [switch]$Force)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here '..\..\..')).Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio s C++ nastroji nenalezeno." }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$env:PATH = "$(Split-Path $vswhere);$env:PATH"

$vkInc = Join-Path $env:TEMP 'bms-dlss-vk'
if (-not (Test-Path "$vkInc\vulkan\vulkan.h")) {
    New-Item -ItemType Directory -Force $vkInc | Out-Null
    Copy-Item "\\?\$(Join-Path $root 'third_party\Vulkan-Headers\include')\*" $vkInc -Recurse -Force
}

$ffx = Join-Path $here 'ffx'
if ($Reference) {
    $gen = Join-Path $root 'tools\spike_afg\gen_ref'
    $outDir = Join-Path $root 'tools\spike_afg\build'
    $lib = Join-Path $outDir 'afg_ref.lib'
    $obj = Join-Path $outDir 'afg_ref_obj'
    $extraDef = '/DAFG_REFERENCE'
    if (-not (Test-Path "$gen\ffx_frameinterpolation_pass_permutations.h")) { throw "Chybi $gen (gen_shaders.ps1 -Reference)" }
} else {
    $gen = Join-Path $here 'gen'
    $outDir = Join-Path $root 'build\x64'
    $lib = Join-Path $outDir 'afg.lib'
    $obj = Join-Path $outDir 'afg_obj'
    $extraDef = ''
}
if (-not (Test-Path "$gen\ffx_frameinterpolation_pass_permutations.h")) { throw "Chybi hlavicky shaderu v $gen (gen_shaders.ps1)" }

if (-not $Force -and (Test-Path $lib)) {
    $t = (Get-Item $lib).LastWriteTimeUtc
    $newer = Get-ChildItem $here, $gen -Recurse -File | Where-Object { $_.LastWriteTimeUtc -gt $t } | Select-Object -First 1
    if (-not $newer) { Write-Host "OK: $lib (aktualni)"; return }
}
New-Item -ItemType Directory -Force $obj | Out-Null

$fork = @('src\backends\vk\ffx_vk.cpp',
          'src\backends\shared\ffx_shader_blobs.cpp',
          'src\backends\shared\blob_accessors\ffx_frameinterpolation_shaderblobs.cpp',
          'src\backends\shared\blob_accessors\ffx_opticalflow_shaderblobs.cpp',
          'src\components\frameinterpolation\ffx_frameinterpolation.cpp',
          'src\components\opticalflow\ffx_opticalflow.cpp',
          'src\shared\ffx_assert.cpp', 'src\shared\ffx_message.cpp', 'src\shared\ffx_object_management.cpp',
          'src\shared\ffx_breadcrumbs_list.cpp') | ForEach-Object { "`"$ffx\$_`"" }
$inc = "/I`"$vkInc`" /I`"$ffx\include`" /I`"$ffx\src\shared`" /I`"$ffx\src\backends\shared`" " +
       "/I`"$ffx\src\components`" /I`"$gen`""
$common = "/nologo /c /O2 /MT /EHsc /std:c++17 /DVK_NO_PROTOTYPES /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS $extraDef"
# Kod AMD se s temito makry preklada na /W3 bez varovani (overeno s MSVC 19.4x); pripadna nova varovani novejsiho
# prekladace vypnout cilene (/wdNNNN) jen tady.
$wdFork = '/W3'
$cmdFork = "cl $common $wdFork /DFFX_FI /DFFX_OF /DUNICODE /D_UNICODE $inc /Fo`"$obj\\`" " + ($fork -join ' ')
$cmdAfg = "cl $common /W3 $inc /Fo`"$obj\\`" `"$here\afg.cpp`""
$cmdLib = "lib /nologo /OUT:`"$lib`" `"$obj\*.obj`""
Remove-Item "$obj\*.obj" -ErrorAction SilentlyContinue
# cl pise jmena souboru; vypsat vse krome nich (varovani a chyby zustanou videt)
$out = cmd /c "call `"$vcvars`" x64 >nul && $cmdFork && $cmdAfg && $cmdLib 2>&1"
$code = $LASTEXITCODE
$out | Where-Object { $_ -notmatch '^\s*((ffx_\w+|afg)\.cpp|Generating Code\.\.\.|Compiling\.\.\.)\s*$' -and $_.Trim() } |
    Write-Host
if ($code -ne 0) { throw "Preklad AFG selhal (kod $code)." }
Write-Host "OK: $lib"
