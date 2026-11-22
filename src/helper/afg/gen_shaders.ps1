# Prelozi GLSL shadery forku FSR 3.1.4 frame interpolation + optical flow (src\helper\afg\ffx) na SPIR-V a vygeneruje
# hlavicky permutaci (*_permutations.h + bloby) pro src\helper\afg\gen. Spousti se jen pri zmene shaderu - vysledek je
# soucasti zdrojaku, bezny build (build_afg.ps1) FidelityFX_SC nepotrebuje.
# Prekladac: third_party\FidelityFX_SC-v1.1.4\FidelityFX_SC.exe (+ jeho glslangValidator.exe), parametry jako
# CMake SDK 1.1.4 (CMakeCompileFrameinterpolationShaders.txt, CMakeCompileOpticalflowShaders.txt, CMakeShaders*.txt
# pro Vulkan). Generuje se jen varianta wave32 bez 16bit (jen tu pristupy k blobum pouzivaji, viz
# ffx\src\backends\shared\blob_accessors).
#   .\gen_shaders.ps1                         fork (upravene shadery s casem t) -> src\helper\afg\gen
#   .\gen_shaders.ps1 -Reference -Out <dir>   nemodifikovane shadery z third_party\FidelityFX-SDK-v1.1.4 (srovnani
#                                             v testu tools\spike_afg; vychozi <dir> = tools\spike_afg\gen_ref)
param([switch]$Reference, [string]$Out)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$sc = Join-Path $root 'third_party\FidelityFX_SC-v1.1.4\FidelityFX_SC.exe'
if (-not (Test-Path $sc)) { throw "Chybi $sc" }
if ($Reference) {
    $sdk = Join-Path $root 'third_party\FidelityFX-SDK-v1.1.4\sdk'
    if (-not $Out) { $Out = Join-Path $root 'tools\spike_afg\gen_ref' }
} else {
    $sdk = Join-Path $PSScriptRoot 'ffx'
    if (-not $Out) { $Out = Join-Path $PSScriptRoot 'gen' }
}
$gpu = Join-Path $sdk 'include\FidelityFX\gpu'
$shaders = Join-Path $sdk 'src\backends\vk\shaders'
New-Item -ItemType Directory -Force $Out | Out-Null
# stare vystupy (jen to, co tento skript generuje)
Get-ChildItem $Out -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^ffx_(frameinterpolation|opticalflow)_.*\.h(\.d)?$' } | Remove-Item

$api = @('-compiler=glslang', '-e', 'CS', '--target-env', 'vulkan1.2', '-S', 'comp', '-Os', '-DFFX_GLSL=1')
$fiBase = @('-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0',
            '-DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0',
            '-DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1',
            '-DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0',
            '-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2', '-reflection', '-DFFX_GPU=1')
$fiPerm = @('-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS={0,1}',
            '-DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS={0,1}',
            '-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH={0,1}')
$ofBase = @('-reflection', '-DFFX_GPU=1')
$ofPerm = @('-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={0,1}')
$ofPasses = 'prepare_luma', 'compute_luminance_pyramid', 'generate_scd_histogram', 'compute_scd_divergence',
            'compute_optical_flow_advanced_pass_v5', 'filter_optical_flow_pass_v5', 'scale_optical_flow_advanced_pass_v5'

$jobs = @()
foreach ($f in Get-ChildItem "$shaders\frameinterpolation\*.glsl") {
    $jobs += , @($f.FullName, ($fiBase + $api + $fiPerm), "$gpu\frameinterpolation")
}
foreach ($p in $ofPasses) {
    $name = if ($p -like '*_v5') { "ffx_opticalflow_$p.glsl" } else { "ffx_opticalflow_${p}_pass.glsl" }
    $jobs += , @((Join-Path "$shaders\opticalflow" $name), ($ofBase + $api + $ofPerm), "$gpu\opticalflow")
}
$n = 0
foreach ($j in $jobs) {
    $src = $j[0]
    if (-not (Test-Path $src)) { throw "Chybi shader $src" }
    $name = [IO.Path]::GetFileNameWithoutExtension($src)
    $a = $j[1] + @("-name=$name", '-DFFX_HALF=0', "-I$gpu", "-I$($j[2])", "-output=$Out", $src)
    # FidelityFX_SC pise jmeno souboru a souhrn na stdout; vypsat jen pri chybe
    $log = & $sc @a 2>&1
    if ($LASTEXITCODE -ne 0) { $log | Write-Host; throw "Preklad $name selhal (kod $LASTEXITCODE)" }
    if (-not (Test-Path (Join-Path $Out "${name}_permutations.h"))) { $log | Write-Host; throw "Chybi vystup ${name}_permutations.h" }
    $n++
}
$files = Get-ChildItem $Out -File | Where-Object { $_.Name -match '^ffx_(frameinterpolation|opticalflow)_.*\.h$' }
Write-Host ("OK: {0} shaderu, {1} souboru, {2:N0} B -> {3}" -f $n, $files.Count, ($files | Measure-Object Length -Sum).Sum, $Out)
