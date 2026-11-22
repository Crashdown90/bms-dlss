# Vrati puvodni DXVK d3d9.dll. Logy a zachycene snimky ponecha.
param([string]$GameDir = "D:\steam\steamapps\common\Black Mesa")
$ErrorActionPreference = 'Stop'
$dst = Join-Path $GameDir 'bin\thirdparty\dxvk-windows-x86'
if (Get-Process bms -ErrorAction SilentlyContinue) { throw "Black Mesa is running - close the game first." }
if (Test-Path "$dst\d3d9_dxvk.dll") {
    Move-Item "$dst\d3d9_dxvk.dll" "$dst\d3d9.dll" -Force
    Write-Host "Original DXVK restored."
} elseif (Test-Path "$dst\d3d9.dll.original") {
    Copy-Item "$dst\d3d9.dll.original" "$dst\d3d9.dll" -Force
    Write-Host "Original DXVK restored from the backup."
} else {
    Write-Host "Nothing to restore (bms-dlss does not seem to be installed). If needed, verify the game files in Steam."
}
# Cast B: hra si mat_viewportscale uklada do config.cfg - bez modu by zmenseny render zustal rozmazany. Vratit 1,
# odebrat cfg, radek exec v autoexec.cfg a bind F4 (ten si hra ulozila do config.cfg).
# Pozor: Steam Cloud synchronizuje cfg/config.cfg a pri dalsim spusteni muze upravu config.cfg prepsat svou kopii.
# Proto se radek exec v autoexec.cfg (ten se nesynchronizuje) nahradi radkem mat_viewportscale 1 - spolehlive.
$cfgDir = Join-Path $GameDir 'bms\cfg'
# 1) autoexec.cfg jako prvni (spolehliva cast); kodovani (BOM) zachovat
$ae = Join-Path $cfgDir 'autoexec.cfg'
try {
    if (Test-Path $ae) {
        $r = New-Object IO.StreamReader($ae, (New-Object Text.UTF8Encoding $false), $true)
        try { $t = $r.ReadToEnd(); $enc = $r.CurrentEncoding } finally { $r.Close() }
        $n = $t -replace '(?m)^// bms-dlss: (upscaling \(cast B\)|power saving mode).*\r?\n', '' -replace '(?m)^[ \t]*exec[ \t]+bms_dlss_upscale\b.*\r?\n?', ''
        if ($n -ne $t) {
            if ($n -notmatch '(?m)^// bms-dlss (odinstalovan|uninstalled)') {
                if ($n.Length -gt 0 -and -not $n.EndsWith("`n")) { $n += "`r`n" }
                $n += "// bms-dlss uninstalled: native render even if Steam Cloud restores an older config.cfg; this line can be deleted`r`nmat_viewportscale 1`r`n"
            }
            [IO.File]::WriteAllText($ae, $n, $enc)
            Write-Host "autoexec.cfg: line 'exec bms_dlss_upscale' replaced with 'mat_viewportscale 1'."
        }
    }
} catch { Write-Host "WARNING: autoexec.cfg cannot be edited ($($_.Exception.Message)) - remove the line 'exec bms_dlss_upscale' manually." }
# 2) config.cfg (Steam Cloud ho muze vratit, proto jen pokus)
$cc = Join-Path $cfgDir 'config.cfg'
try {
    if (Test-Path $cc) {
        $t = [IO.File]::ReadAllText($cc)
        $n = $t -replace 'mat_viewportscale\s+"[^"]*"', 'mat_viewportscale "1"' -replace '(?m)^bind\s+"[^"]*"\s+"dlss_vp"\r?\n', ''
        if ($n -ne $t) { [IO.File]::WriteAllText($cc, $n, (New-Object Text.UTF8Encoding $false)); Write-Host "config.cfg: mat_viewportscale 1, bind dlss_vp removed." }
    }
} catch { Write-Host "WARNING: config.cfg cannot be edited ($($_.Exception.Message)) - if needed type in the game console: mat_viewportscale 1" }
try { if (Test-Path (Join-Path $cfgDir 'bms_dlss_upscale.cfg')) { Remove-Item (Join-Path $cfgDir 'bms_dlss_upscale.cfg') -Force } } catch { Write-Host "WARNING: bms_dlss_upscale.cfg cannot be deleted." }
# Helper modu (nastaveni bms_dlss.ini a logy zustavaji)
if (Test-Path "$dst\bms_dlss_helper.exe") { Remove-Item "$dst\bms_dlss_helper.exe" -Force; Write-Host "bms_dlss_helper.exe removed." }
# Runtime DLSS, ktery pridal install.ps1 (hra ho sama nema)
if (Test-Path "$dst\nvngx_dlss.dll") {
    Remove-Item "$dst\nvngx_dlss.dll" -Force
    Write-Host "nvngx_dlss.dll removed."
}
# Runtime DLSS Frame Generation (install.ps1)
if (Test-Path "$dst\nvngx_dlssg.dll") {
    Remove-Item "$dst\nvngx_dlssg.dll" -Force
    Write-Host "nvngx_dlssg.dll removed."
}
# Runtime AMD FSR (install.ps1); pripadny cizi soubor stejneho jmena vratit ze zalohy
foreach ($name in @('amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll',
                    'amd_fidelityfx_framegeneration_vk.dll')) {
    $t = Join-Path $dst $name
    if (Test-Path $t) { Remove-Item $t -Force; Write-Host "$name removed." }
    if (Test-Path "$t.bms-dlss-zaloha") { Move-Item "$t.bms-dlss-zaloha" $t -Force; Write-Host "$name restored from the backup." }
}
# x64 Visual C++ runtime pro DLL AMD (install.ps1, podslozka vcrt); slozka se smaze jen kdyz je pak prazdna
foreach ($name in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    $t = Join-Path $dst "vcrt\$name"
    if (Test-Path $t) { Remove-Item $t -Force; Write-Host "vcrt\$name removed." }
}
if ((Test-Path "$dst\vcrt") -and -not (Get-ChildItem "$dst\vcrt" -Force)) { Remove-Item "$dst\vcrt" -Force }
