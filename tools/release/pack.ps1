# bms-dlss: slozi balicek vydani dist\bms-dlss-<verze>.zip z build\ a sablon (nic nepreklada, build.ps1 spust predem).
#
# Rozlozeni (-Layout):
#   kit  (vychozi) - vse ve slozce bms-dlss\ ; hrac zip rozbali do slozky hry a spusti bms-dlss\install.bat
#                    (bms-dlss-setup.ps1 najde hru a zavola nezmeneny install.ps1 -GameDir). DXVK se nebali,
#                    pouzije se DXVK hry (install.ps1 ho prejmenuje na d3d9_dxvk.dll).
#   game           - "rozbal do korene hry": soubory primo na cilovych cestach jako po install.ps1. Proxy d3d9.dll
#                    by prepsala DXVK hry, proto je nutne -DxvkDll (oficialni 32bitove d3d9.dll z vydani DXVK),
#                    ktere se zabali jako d3d9_dxvk.dll. Bez autoexec radku (F4) - ten jen popsat v INSTALL.
#
# Do zipu nikdy: docs\, PROGRESS.md, tools\, zalohy\, third_party\ (krome runtime DLL z build\), *.pdb, *.obj,
# *.lib, *.exp, logy, zalohy *.vNN, *.original. Kontroluje podpisy DLL NVIDIA/AMD, release (ne dev) DLSS-G,
# znacku proxy, architekturu, VERSIONINFO a zakazana slova. Vypise SHA256 zipu i vsech souboru (SHA256SUMS.txt).
#
# Priklady:
#   powershell -ExecutionPolicy Bypass -File tools\release\pack.ps1
#   powershell -ExecutionPolicy Bypass -File tools\release\pack.ps1 -Release
#   powershell -ExecutionPolicy Bypass -File tools\release\pack.ps1 -Layout game -DxvkDll C:\dl\dxvk-3.1.1\x32\d3d9.dll
param(
    [string]$Version = '',
    [ValidateSet('kit', 'game')][string]$Layout = 'kit',
    [string]$BuildDir = '',
    [string]$OutDir = '',
    [string]$Readme = '',
    [string]$DxvkDll = '',
    [switch]$Release,   # prisny rezim: chyba misto varovani (zastupne <autor>, chybejici VERSIONINFO, nesoulad verzi)
    [switch]$Force      # prepsat existujici zip
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build' }
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }
if (-not $Readme) { $Readme = Join-Path $root 'README.md' }
$script:warnings = 0
function Warn([string]$m) { if ($Release) { throw "CHYBA (-Release): $m" } ; Write-Host "VAROVANI: $m"; $script:warnings++ }

# ---- verze (jediny zdroj: src\res\version.h) ----
$verH = Join-Path $root 'src\res\version.h'
if (-not $Version) {
    $m = Select-String -Path $verH -Pattern '#define\s+BMSDLSS_VER_STR\s+"([^"]+)"' | Select-Object -First 1
    if (-not $m) { throw "Verze nenalezena v $verH (BMSDLSS_VER_STR)." }
    $Version = $m.Matches[0].Groups[1].Value
}
if ($Version -notmatch '^\d+\.\d+\.\d+([.-][0-9A-Za-z.-]+)?$') { throw "Neplatna verze '$Version'." }
Write-Host "bms-dlss $Version, rozlozeni $Layout"

# ---- vstupy ----
$bins = @('d3d9.dll', 'bms_dlss_helper.exe', 'nvngx_dlss.dll', 'nvngx_dlssg.dll',
          'amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll', 'amd_fidelityfx_framegeneration_vk.dll',
          'vcrt\msvcp140.dll', 'vcrt\vcruntime140.dll', 'vcrt\vcruntime140_1.dll')
foreach ($b in $bins) { if (-not (Test-Path (Join-Path $BuildDir $b))) { throw "Chybi $BuildDir\$b - spust build.ps1." } }
$texts = @{
    'README.md'              = $Readme
    'LICENSE'                = (Join-Path $root 'LICENSE')
    'THIRD_PARTY_NOTICES.md' = (Join-Path $root 'THIRD_PARTY_NOTICES.md')
}
foreach ($k in @($texts.Keys)) { if (-not (Test-Path $texts[$k])) { throw "Chybi $($texts[$k])." } }
foreach ($f in 'bms_dlss.ini', 'bms_dlss_upscale.cfg', 'install.ps1', 'uninstall.ps1') {
    if (-not (Test-Path (Join-Path $root $f))) { throw "Chybi $root\$f." }
}

function Get-PeMachine([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ([Text.Encoding]::ASCII.GetString($b, $pe, 4) -ne "PE`0`0") { throw "$path neni PE soubor." }
    return [BitConverter]::ToUInt16($b, $pe + 4)
}
function Test-Contains([string]$path, [string]$text) {
    [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path)).Contains($text)
}

# ---- kontroly binarek ----
$proxy = Join-Path $BuildDir 'd3d9.dll'
$helper = Join-Path $BuildDir 'bms_dlss_helper.exe'
if (-not (Test-Contains $proxy 'BMS_DLSS_PROXY_MARKER_v1')) { throw "$proxy nema znacku proxy (BMS_DLSS_PROXY_MARKER_v1)." }
if ((Get-PeMachine $proxy) -ne 0x14C) { throw "$proxy neni 32bitova (x86)." }
if ((Get-PeMachine $helper) -ne 0x8664) { throw "$helper neni 64bitovy (x64)." }
foreach ($b in $bins | Where-Object { $_ -match '^(nvngx|amd_|vcrt\\)' }) {
    $p = Join-Path $BuildDir $b
    $sig = Get-AuthenticodeSignature $p
    $want = if ($b -like 'nvngx*') { 'NVIDIA Corporation' } elseif ($b -like 'vcrt\*') { 'Microsoft Corporation' } else { 'Advanced Micro Devices' }
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch [regex]::Escape($want)) {
        throw "$b nema platny podpis $want (stav $($sig.Status))."
    }
    if ((Get-PeMachine $p) -ne 0x8664) { throw "$b neni x64." }
}
$dev = Join-Path $root 'third_party\DLSS\lib\Windows_x86_64\dev\nvngx_dlssg.dll'
if ((Test-Path $dev) -and (Get-FileHash $dev).Hash -eq (Get-FileHash (Join-Path $BuildDir 'nvngx_dlssg.dll')).Hash) {
    throw 'build\nvngx_dlssg.dll je vyvojova (dev) knihovna - do balicku smi jen release.'
}
foreach ($p in $proxy, $helper) {
    $vi = (Get-Item $p).VersionInfo
    if ($vi.ProductName -ne 'bms-dlss') { Warn "$(Split-Path $p -Leaf) nema VERSIONINFO (src\res\*.rc, postup docs\priprava-vydani.md)." }
    elseif ($vi.ProductVersion -ne $Version) { Warn "$(Split-Path $p -Leaf): ProductVersion $($vi.ProductVersion) <> $Version." }
    elseif ($vi.CompanyName -match '<autor>') { Warn "$(Split-Path $p -Leaf): CompanyName je zastupne <autor>." }
}
if ((Get-Content (Join-Path $root 'LICENSE') -Raw) -match '<autor>') { Warn 'LICENSE obsahuje zastupne <autor>.' }
foreach ($p in $proxy, $helper) {
    if ((Get-AuthenticodeSignature $p).Status -ne 'Valid') { Write-Host "Info: $(Split-Path $p -Leaf) neni podepsana (podpis kodu = rozhodnuti autora)." }
}

$dxvkVer = ''
if ($Layout -eq 'game') {
    if (-not $DxvkDll) {
        throw ("-Layout game potrebuje -DxvkDll (oficialni 32bitove d3d9.dll z vydani DXVK, github.com/doitsujin/dxvk). " +
               "Bez nej by rozbaleni prepsalo DXVK hry a proxy by nemela co volat. Jinak pouzij vychozi -Layout kit.")
    }
    if (-not (Test-Path $DxvkDll)) { throw "Nenalezeno $DxvkDll." }
    if ((Get-PeMachine $DxvkDll) -ne 0x14C) { throw "$DxvkDll neni 32bitova (x32 / dxvk-windows-x86)." }
    if (Test-Contains $DxvkDll 'BMS_DLSS_PROXY_MARKER_v1') { throw "$DxvkDll je proxy bms-dlss, ne DXVK." }
    $s = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($DxvkDll))
    if (-not $s.Contains('Direct3DCreate9') -or $s -notmatch '(?i)dxvk') { throw "$DxvkDll nevypada jako d3d9.dll z DXVK." }
    $vm = [regex]::Match($s, 'v\d+\.\d+(\.\d+)?')
    $dxvkVer = if ($vm.Success) { $vm.Value } else { '(verze?)' }
    Write-Host "DXVK do balicku: $DxvkDll ($dxvkVer, SHA256 $((Get-FileHash $DxvkDll).Hash))"
}

# ---- staging (nova docasna slozka, po zabaleni se smaze jen ona) ----
$stage = Join-Path ([IO.Path]::GetTempPath()) ("bms-dlss-pack-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $stage | Out-Null
$files = New-Object System.Collections.Generic.List[string]
function Put([string]$src, [string]$rel, [switch]$Crlf, [hashtable]$Replace) {
    $dst = Join-Path $stage $rel
    New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
    if ($Crlf -or $Replace) {
        $t = [IO.File]::ReadAllText($src)
        if ($Replace) { foreach ($k in $Replace.Keys) { $t = $t.Replace($k, $Replace[$k]) } }
        if ($Crlf) { $t = ($t -replace "`r`n", "`n") -replace "`n", "`r`n" }
        $bom = [IO.Path]::GetExtension($src) -eq '.txt'
        [IO.File]::WriteAllText($dst, $t, (New-Object Text.UTF8Encoding $bom))
    } else {
        Copy-Item -LiteralPath $src -Destination $dst
    }
    $files.Add(($rel -replace '\\', '/'))
}

try {
    $kitDir = Join-Path $PSScriptRoot 'kit'
    if ($Layout -eq 'kit') {
        $p = 'bms-dlss'
        foreach ($b in $bins) { Put (Join-Path $BuildDir $b) "$p\build\$b" }
        Put (Join-Path $root 'bms_dlss.ini') "$p\bms_dlss.ini"
        Put (Join-Path $root 'bms_dlss_upscale.cfg') "$p\bms_dlss_upscale.cfg"
        Put (Join-Path $root 'install.ps1') "$p\install.ps1"
        Put (Join-Path $root 'uninstall.ps1') "$p\uninstall.ps1"
        Put (Join-Path $kitDir 'bms-dlss-setup.ps1') "$p\bms-dlss-setup.ps1" -Crlf
        Put (Join-Path $kitDir 'install.bat') "$p\install.bat" -Crlf
        Put (Join-Path $kitDir 'uninstall.bat') "$p\uninstall.bat" -Crlf
        Put (Join-Path $PSScriptRoot 'INSTALL.txt') "$p\INSTALL.txt" -Crlf -Replace @{ '{VERSION}' = $Version }
    } else {
        $d = 'bin\thirdparty\dxvk-windows-x86'
        foreach ($b in $bins) { Put (Join-Path $BuildDir $b) "$d\$b" }
        Put $DxvkDll "$d\d3d9_dxvk.dll"
        Put (Join-Path $root 'bms_dlss.ini') "$d\bms_dlss.ini"
        Put (Join-Path $root 'bms_dlss_upscale.cfg') 'bms\cfg\bms_dlss_upscale.cfg'
        $p = 'bms-dlss'
        Put (Join-Path $root 'third_party\dxvk-interop\LICENSE') "$p\DXVK-LICENSE.txt" -Crlf
        Put (Join-Path $PSScriptRoot 'INSTALL-game.txt') "$p\INSTALL.txt" -Crlf -Replace @{ '{VERSION}' = $Version; '{DXVK}' = $dxvkVer }
    }
    foreach ($k in $texts.Keys) { Put $texts[$k] "bms-dlss\$k" }

    # ---- pojistka: nic zakazaneho ----
    $bad = $files | Where-Object {
        $_ -match '(?i)(^|/)(docs|tools|zalohy|third_party)/' -or $_ -match '(?i)PROGRESS\.md$' -or
        $_ -match '(?i)\.(pdb|obj|lib|exp|ilk|log|prev|original|bak|zaloha)$' -or $_ -match '(?i)\.v\d+[a-z0-9]*$' -or
        $_ -match '(?i)\.user-|\.pre-|\.default\.'
    }
    if ($bad) { throw "Zakazane soubory v balicku: $($bad -join ', ')" }

    # ---- zakazana slova (texty + nase binarky; DLL NVIDIA/AMD se nekontroluji). Seznam je jen lokalne
    #      (tools\release\local\, .gitignore): kazdy radek = regularni vyraz, radek "?" na zacatku = jen upozornit. ----
    $listFile = Join-Path $PSScriptRoot 'local\zakazana-slova.txt'
    $denyList = @(); $softList = @()
    if (Test-Path $listFile) {
        foreach ($l in [IO.File]::ReadAllLines($listFile)) {
            $l = $l.Trim()
            if (-not $l -or $l.StartsWith('#')) { continue }
            if ($l.StartsWith('?')) { $softList += $l.Substring(1).Trim() } else { $denyList += $l }
        }
    } else { Warn "Chybi $listFile - kontrola zakazanych slov preskocena." }
    $deny = if ($denyList) { ($denyList | ForEach-Object { "(?:$_)" }) -join '|' } else { '(?!)' }
    $soft = if ($softList) { ($softList | ForEach-Object { "(?:$_)" }) -join '|' } else { '(?!)' }
    foreach ($rel in $files) {
        $full = Join-Path $stage ($rel -replace '/', '\')
        $leaf = Split-Path $full -Leaf
        if ($leaf -match '^(nvngx|amd_|d3d9_dxvk|msvcp140|vcruntime140)') { continue }
        if ($leaf -match '\.(dll|exe)$') {
            $raw = [IO.File]::ReadAllBytes($full)
            # jen "lidske" retezce (aspon 8 znaku, mezera, prevazne pismena) - jinak nahodne bajty kodu davaji falesne shody
            $lines = @([regex]::Matches([Text.Encoding]::ASCII.GetString($raw), '[\x20-\x7E]{8,}') | ForEach-Object Value) +
                     @([regex]::Matches([Text.Encoding]::Unicode.GetString($raw), '[\x20-\x7E\u00C0-\u017F]{8,}') | ForEach-Object Value)
            $lines = @($lines | Where-Object { $_.Contains(' ') -and ([regex]::Matches($_, '[A-Za-z]').Count -ge $_.Length * 0.6) })
        } else {
            $lines = [IO.File]::ReadAllLines($full)
        }
        $n = 0
        foreach ($l in $lines) {
            $n++
            if ($l -cmatch $deny) { throw "Zakazane slovo v $rel (radek/retezec $n): $($l.Trim())" }
            if ($leaf -notmatch '\.(dll|exe)$' -and $l -match $soft) { Write-Host "Info: $rel radek $n obsahuje '$($Matches[0])' - zkontroluj kontext: $($l.Trim())" }
        }
    }

    # ---- SHA256SUMS.txt ----
    $sums = foreach ($rel in ($files | Sort-Object)) {
        "{0} *{1}" -f (Get-FileHash -Algorithm SHA256 (Join-Path $stage ($rel -replace '/', '\'))).Hash.ToLower(), $rel
    }
    $sumRel = 'bms-dlss/SHA256SUMS.txt'
    [IO.File]::WriteAllText((Join-Path $stage 'bms-dlss\SHA256SUMS.txt'), (($sums -join "`r`n") + "`r`n"), (New-Object Text.UTF8Encoding $false))
    $files.Add($sumRel)

    # ---- zip (lomitka /, cas souboru zachovan) ----
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $name = if ($Layout -eq 'kit') { "bms-dlss-$Version.zip" } else { "bms-dlss-$Version-game.zip" }
    $zipPath = Join-Path $OutDir $name
    if (Test-Path $zipPath) {
        if (-not $Force) { throw "$zipPath uz existuje (prepsat: -Force)." }
        Remove-Item -LiteralPath $zipPath -Force
    }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($rel in ($files | Sort-Object)) {
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $stage ($rel -replace '/', '\')), $rel,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $zip.Dispose() }

    # ---- overeni: znovu otevrit zip a porovnat obsah se SHA256SUMS ----
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entries = @($zip.Entries | ForEach-Object FullName)
        if ($entries.Count -ne $files.Count) { throw "Zip ma $($entries.Count) polozek, ocekavano $($files.Count)." }
        if ($entries | Where-Object { $_ -match '\\' }) { throw 'Zip obsahuje zpetna lomitka v cestach.' }
        $sha = [Security.Cryptography.SHA256]::Create()
        foreach ($line in $sums) {
            $h, $rel = $line -split ' \*', 2
            $e = $zip.GetEntry($rel)
            if (-not $e) { throw "V zipu chybi $rel." }
            $st = $e.Open()
            try { $eh = -join ($sha.ComputeHash($st) | ForEach-Object { $_.ToString('x2') }) } finally { $st.Dispose() }
            if ($eh -ne $h) { throw "Hash $rel v zipu nesedi." }
        }
    } finally { $zip.Dispose() }

    $zipHash = (Get-FileHash -Algorithm SHA256 $zipPath).Hash.ToLower()
    [IO.File]::WriteAllText("$zipPath.sha256", "$zipHash *$name`r`n", (New-Object Text.UTF8Encoding $false))
    Write-Host ''
    Write-Host "Obsah ($($files.Count) souboru):"
    foreach ($line in $sums) { Write-Host "  $line" }
    Write-Host ''
    Write-Host ("Balicek: {0} ({1:N1} MB)" -f $zipPath, ((Get-Item $zipPath).Length / 1MB))
    Write-Host "SHA256:  $zipHash"
    if ($script:warnings) { Write-Host "Varovani: $($script:warnings) (pro vydani spust s -Release)." }
} finally {
    if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
