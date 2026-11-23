# Vrati puvodni DXVK d3d9.dll. Logy, bms_dlss.ini a zaloha d3d9.dll.original zustavaji.
# Spousti ho uninstall.bat z balicku (bms-dlss-setup.ps1 najde hru a preda -GameDir).
param([string]$GameDir = '')
$ErrorActionPreference = 'Stop'
if (-not $GameDir) { throw 'Run bms-dlss\uninstall.bat from the release package (or pass -GameDir "<Black Mesa folder>").' }
$marker = 'BMS_DLSS_PROXY_MARKER_v1'
$dst = Join-Path $GameDir 'bin\thirdparty\dxvk-windows-x86'
if (Get-Process bms -ErrorAction SilentlyContinue) { throw "Black Mesa is running - close the game first." }
function IsProxy($f) { (Test-Path $f) -and [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($f)).Contains($marker) }
# DXVK vracet jen kdyz je d3d9.dll nase proxy (nebo chybi). Kdyz uz tam je DXVK (Steam ho po aktualizaci hry nebo
# overeni souboru vratil), nechat ho - zaloha z doby instalace muze byt starsi verze.
if ((IsProxy "$dst\d3d9.dll") -or -not (Test-Path "$dst\d3d9.dll")) {
    if (Test-Path "$dst\d3d9_dxvk.dll") {
        Move-Item "$dst\d3d9_dxvk.dll" "$dst\d3d9.dll" -Force
        Write-Host "Original DXVK restored."
    } elseif (Test-Path "$dst\d3d9.dll.original") {
        Copy-Item "$dst\d3d9.dll.original" "$dst\d3d9.dll" -Force
        Write-Host "Original DXVK restored from the backup d3d9.dll.original."
    } else {
        Write-Host "WARNING: no DXVK backup found - verify the game files in Steam (Properties > Installed Files)."
    }
} else {
    Write-Host "d3d9.dll is already the game's DXVK (e.g. restored by a Steam update) - left unchanged."
    # Kopie DXVK z doby instalace (pridal install.ps1) uz neni potreba a mohla by byt starsi
    if (Test-Path "$dst\d3d9_dxvk.dll") { Remove-Item "$dst\d3d9_dxvk.dll" -Force; Write-Host "Old copy d3d9_dxvk.dll removed." }
}
# Cast B: hra si mat_viewportscale uklada do config.cfg - bez modu by zmenseny render zustal rozmazany. Vratit 1,
# odebrat cfg, radek exec v autoexec.cfg a bind F4 (ten si hra ulozila do config.cfg).
# Pozor: Steam Cloud synchronizuje cfg/config.cfg a pri dalsim spusteni muze upravu config.cfg prepsat svou kopii.
# Proto se radek exec v autoexec.cfg (ten se nesynchronizuje) nahradi radkem mat_viewportscale 1 - spolehlive.
$cfgDir = Join-Path $GameDir 'bms\cfg'
# Puvodni prirazeni F4 (install.ps1 ho uklada do komentare v autoexec.cfg): pri odinstalaci ho vratit
$prevF4 = $null
# 1) autoexec.cfg jako prvni (spolehliva cast); kodovani (BOM) zachovat
$ae = Join-Path $cfgDir 'autoexec.cfg'
try {
    if (Test-Path $ae) {
        $r = New-Object IO.StreamReader($ae, (New-Object Text.UTF8Encoding $false), $true)
        try { $t = $r.ReadToEnd(); $enc = $r.CurrentEncoding } finally { $r.Close() }
        $m = [regex]::Match($t, '(?m)^// bms-dlss: previous F4 binding: (.*?)\r?$')
        if ($m.Success) { $prevF4 = $m.Groups[1].Value }
        $n = $t -replace '(?m)^// bms-dlss: (upscaling \(cast B\)|power saving mode|previous F4 binding).*\r?\n?', '' -replace '(?m)^[ \t]*exec[ \t]+bms_dlss_upscale\b.*\r?\n?', ''
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
# 2) config.cfg (Steam Cloud ho muze vratit, proto jen pokus). Bind F4 modu nahradit puvodnim, jinak odebrat.
$cc = Join-Path $cfgDir 'config.cfg'
try {
    if (Test-Path $cc) {
        $t = [IO.File]::ReadAllText($cc)
        $n = $t -replace 'mat_viewportscale\s+"[^"]*"', 'mat_viewportscale "1"'
        if ($prevF4) {
            $n = $n -replace '(?m)^bind\s+"F4"\s+"dlss_vp"', ($prevF4 -replace '\$', '$$$$')
            $n = $n -replace '(?m)^bind\s+"[^"]*"\s+"dlss_vp"\r?\n', ''
        } else {
            $n = $n -replace '(?m)^bind\s+"[^"]*"\s+"dlss_vp"\r?\n', ''
        }
        if ($n -ne $t) {
            [IO.File]::WriteAllText($cc, $n, (New-Object Text.UTF8Encoding $false))
            Write-Host ("config.cfg: mat_viewportscale 1, " + $(if ($prevF4) { "F4 restored to: $prevF4" } else { 'bind dlss_vp removed.' }))
        }
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
Write-Host "Settings (bms_dlss.ini), logs and the backup d3d9.dll.original stay in $dst - delete them manually if you want."
