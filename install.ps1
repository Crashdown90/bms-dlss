# Nainstaluje proxy d3d9.dll do Black Mesa (pouze pro volbu "Hrat DXVK").
# Puvodni DXVK se zachova jako d3d9_dxvk.dll a zaloha d3d9.dll.original.
param([string]$GameDir = "D:\steam\steamapps\common\Black Mesa")
$ErrorActionPreference = 'Stop'
$marker = 'BMS_DLSS_PROXY_MARKER_v1'
$dst = Join-Path $GameDir 'bin\thirdparty\dxvk-windows-x86'
$src = Join-Path $PSScriptRoot 'build\d3d9.dll'

if (Get-Process bms -ErrorAction SilentlyContinue) { throw "Black Mesa is running - close the game first." }
if (-not (Test-Path $src)) { throw "Missing $src - run build.ps1 first." }
if (-not (Test-Path "$dst\d3d9.dll")) { throw "Not found: $dst\d3d9.dll" }

function IsProxy($f) { [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($f)).Contains($marker) }

if (IsProxy "$dst\d3d9.dll") {
    if (-not (Test-Path "$dst\d3d9_dxvk.dll")) { throw "d3d9.dll is the bms-dlss proxy but d3d9_dxvk.dll is missing - verify the game files in Steam." }
    Write-Host "bms-dlss is already installed - updating it."
} else {
    # Puvodni (nebo Steamem aktualizovane) DXVK
    Copy-Item "$dst\d3d9.dll" "$dst\d3d9_dxvk.dll" -Force
    Copy-Item "$dst\d3d9.dll" "$dst\d3d9.dll.original" -Force
    Write-Host "Original DXVK kept as d3d9_dxvk.dll (+ backup d3d9.dll.original)."
}
Copy-Item $src "$dst\d3d9.dll" -Force
$helper = Join-Path $PSScriptRoot 'build\bms_dlss_helper.exe'
if (Test-Path $helper) { Copy-Item $helper "$dst\bms_dlss_helper.exe" -Force }
# Runtime NVIDIA DLSS pro helper (DLAA); hleda se vedle bms_dlss_helper.exe
$ngx = Join-Path $PSScriptRoot 'build\nvngx_dlss.dll'
if (Test-Path $ngx) { Copy-Item $ngx "$dst\nvngx_dlss.dll" -Force }
# Runtime NVIDIA DLSS Frame Generation (DLSS-G, [fg] Mode=dlssg): release knihovna z buildu (build.ps1 bere jen
# lib/.../rel z DLSS SDK; vyvojova "dev" se neinstaluje nikdy). Bez ni helper FG ohlasi jako nedostupne.
$ngxg = Join-Path $PSScriptRoot 'build\nvngx_dlssg.dll'
if (Test-Path $ngxg) { Copy-Item $ngxg "$dst\nvngx_dlssg.dll" -Force }
else { Write-Host "WARNING: $ngxg is missing - DLSS Frame Generation will not work." }
# Runtime AMD FSR pro helper ([upscale] Upscaler=fsr / auto, [fg] Mode=fsr2x): podepsany loader ffx API + upscaler
# + frame generation, taky vedle helperu.
# Cizi soubor stejneho jmena (hra ho nema, ale pro jistotu) se jednou zazalohuje jako .bms-dlss-zaloha.
foreach ($name in @('amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll',
                    'amd_fidelityfx_framegeneration_vk.dll')) {
    $f = Join-Path $PSScriptRoot "build\$name"
    if (-not (Test-Path $f)) {
        if ($name -match 'framegeneration') { Write-Host "WARNING: $f is missing - FSR Frame Generation will not work." }
        else { Write-Host "WARNING: $f is missing - FSR will not work (DLSS is used)." }
        continue
    }
    $t = Join-Path $dst $name
    if ((Test-Path $t) -and -not (Test-Path "$t.bms-dlss-zaloha") -and
        (Get-FileHash $t).Hash -ne (Get-FileHash $f).Hash -and
        (Get-AuthenticodeSignature $t).SignerCertificate.Subject -notmatch 'Advanced Micro Devices') {
        Copy-Item $t "$t.bms-dlss-zaloha" -Force
        Write-Host "Original $name saved as $name.bms-dlss-zaloha."
    }
    Copy-Item $f $t -Force
}
# x64 Visual C++ runtime pro DLL AMD do podslozky vcrt (helper ho pouzije jen kdyz v systemu chybi nebo je stary)
$vcrtSrc = Join-Path $PSScriptRoot 'build\vcrt'
if (Test-Path (Join-Path $vcrtSrc 'msvcp140.dll')) {
    New-Item -ItemType Directory -Force "$dst\vcrt" | Out-Null
    foreach ($name in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        $f = Join-Path $vcrtSrc $name
        if (Test-Path $f) { Copy-Item $f "$dst\vcrt\$name" -Force }
    }
} else { Write-Host "WARNING: $vcrtSrc is missing - FSR needs the Microsoft Visual C++ Redistributable x64." }
$ini = Join-Path $PSScriptRoot 'bms_dlss.ini'
# Uzivatelovo nastaveni se nikdy neprepisuje. Nova sablona (s pripadnymi novymi klici)
# se ulozi vedle jako bms_dlss.ini.default; chybejici klice proxy i helper doplni vychozimi hodnotami.
if (Test-Path "$dst\bms_dlss.ini") {
    Copy-Item $ini "$dst\bms_dlss.ini.default" -Force
    Write-Host "bms_dlss.ini kept unchanged (new template: bms_dlss.ini.default)."
} else {
    Copy-Item $ini "$dst\bms_dlss.ini" -Force
}
# Cast B (skutecny upscaling): cfg s aliasy a klavesou F4 + radek exec v autoexec.cfg. Po startu hry vzdy nativni
# rozliseni (mat_viewportscale se uklada do config.cfg, bez modu by zmenseny render jen roztahl engine).
$cfgDir = Join-Path $GameDir 'bms\cfg'
if (Test-Path $cfgDir) {
    Copy-Item (Join-Path $PSScriptRoot 'bms_dlss_upscale.cfg') (Join-Path $cfgDir 'bms_dlss_upscale.cfg') -Force
    $ae = Join-Path $cfgDir 'autoexec.cfg'
    $line = 'exec bms_dlss_upscale'
    # Text i kodovani autoexecu (BOM: UTF-8 / UTF-16 z Poznamkoveho bloku) zachovat, jen pripsat na konec
    $t = ''; $enc = New-Object Text.UTF8Encoding $false
    if (Test-Path $ae) {
        $r = New-Object IO.StreamReader($ae, $enc, $true)
        try { $t = $r.ReadToEnd(); $enc = $r.CurrentEncoding } finally { $r.Close() }
    }
    if ($t -notmatch '(?m)^[ \t]*exec[ \t]+bms_dlss_upscale\b') {
        if ($t.Length -gt 0 -and -not $t.EndsWith("`n")) { $t += "`r`n" }
        $t += "// bms-dlss: power saving mode (key F4); removed by uninstall.ps1`r`n$line`r`n"
        [IO.File]::WriteAllText($ae, $t, $enc)
        Write-Host "autoexec.cfg: added line '$line' (key F4: native -> Quality -> Balanced, experimental)."
    }
    # Upozornit, kdyz ma hrac na F4 vlastni bind (nas cfg ho prepise)
    $cc = Join-Path $cfgDir 'config.cfg'
    if (Test-Path $cc) {
        $b = Select-String -Path $cc -Pattern '^\s*bind\s+"F4"\s+"([^"]*)"' | Select-Object -First 1
        if ($b -and $b.Matches[0].Groups[1].Value -ne 'dlss_vp') {
            Write-Host "WARNING: F4 was bound to '$($b.Matches[0].Groups[1].Value)' - it is now bound to dlss_vp (power saving mode)."
        }
    }
} else {
    Write-Host "WARNING: $cfgDir not found - the F4 power saving mode is not set up."
}
Write-Host "Done: bms-dlss installed to $dst"
