# bms-dlss: install / uninstall from the release package. Finds the Black Mesa folder and runs install.ps1 or
# uninstall.ps1 (next to this script) with -GameDir. Started by install.bat / uninstall.bat.
param(
    [ValidateSet('install', 'uninstall')][string]$Action = 'install',
    [string]$GameDir = ''
)
$ErrorActionPreference = 'Stop'
$appId = '362890'

function Test-GameDir([string]$d) { $d -and (Test-Path (Join-Path $d 'bms.exe')) }

function Find-GameDir {
    # 1) package extracted into the game folder: <game>\bms-dlss\bms-dlss-setup.ps1
    $parent = Split-Path $PSScriptRoot -Parent
    if (Test-GameDir $parent) { return $parent }
    # 2) Steam libraries (registry + libraryfolders.vdf; both files are UTF-8)
    $steam = $null
    foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        try {
            $p = Get-ItemProperty $k -ErrorAction Stop
            if ($p.SteamPath) { $steam = $p.SteamPath; break }
            if ($p.InstallPath) { $steam = $p.InstallPath; break }
        } catch {}
    }
    if (-not $steam) { return $null }
    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw -Encoding UTF8), '"path"\s+"([^"]+)"')) {
            $libs += ($m.Groups[1].Value -replace '\\\\', '\')
        }
    }
    foreach ($lib in ($libs | Select-Object -Unique)) {
        $manifest = Join-Path $lib "steamapps\appmanifest_$appId.acf"
        if (-not (Test-Path $manifest)) { continue }
        $dir = 'Black Mesa'
        $m = Select-String -Path $manifest -Encoding UTF8 -Pattern '"installdir"\s+"([^"]+)"' | Select-Object -First 1
        if ($m) { $dir = $m.Matches[0].Groups[1].Value }
        $g = Join-Path $lib "steamapps\common\$dir"
        if (Test-GameDir $g) { return $g }
    }
    return $null
}

try {
    if (-not $GameDir) { $GameDir = Find-GameDir }
    if (-not (Test-GameDir $GameDir)) {
        throw ("Black Mesa not found. Extract the package into the game folder (Steam > Black Mesa > Manage > Browse " +
               "local files) or run: bms-dlss-setup.ps1 $Action -GameDir `"<game folder>`"`n" +
               "CZ: Hra nenalezena. Rozbal balik do slozky hry (Steam > Black Mesa > Spravovat > Prochazet mistni soubory).")
    }
    Write-Host "Black Mesa: $GameDir"

    $dxvk = Join-Path $GameDir 'bin\thirdparty\dxvk-windows-x86'
    if ($Action -eq 'install' -and -not (Test-Path (Join-Path $dxvk 'd3d9.dll'))) {
        throw ("The game has no DXVK folder ($dxvk). bms-dlss needs the current Steam version of Black Mesa for Windows " +
               "with the launch option 'DXVK'. Update the game or verify its files in Steam and try again.`n" +
               "CZ: Hra nema slozku DXVK. Mod potrebuje aktualni verzi Black Mesa ze Steamu - aktualizuj hru nebo over soubory.")
    }
    if (Get-Process bms -ErrorAction SilentlyContinue) {
        throw "Black Mesa is running - close the game first.`nCZ: Black Mesa bezi - nejdriv zavri hru."
    }

    $script = Join-Path $PSScriptRoot "$Action.ps1"
    if (-not (Test-Path $script)) {
        throw "Missing $script - extract the whole zip again.`nCZ: Chybi soubor - rozbal znovu cely zip."
    }
    & $script -GameDir $GameDir
} catch {
    Write-Host ''
    Write-Host "$(if ($Action -eq 'install') { 'INSTALL' } else { 'UNINSTALL' }) FAILED: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "CZ: $(if ($Action -eq 'install') { 'Instalace' } else { 'Odinstalace' }) se nepovedla - viz hlaska vyse."
    exit 1
}

Write-Host ''
if ($Action -eq 'install') {
    Write-Host 'Done. Start the game with the Steam launch option "Play DXVK" (without it the mod is not loaded).'
    Write-Host 'Turn off anti-aliasing (MSAA) in the game options. In game: Alt+N = bms-dlss menu, Alt+F8 = mod on/off,'
    Write-Host 'F4 = power saving mode (experimental).'
    Write-Host 'CZ: Hotovo. Hru spoustej volbou Steamu "Hrat DXVK" (bez ni se mod nenacte). Ve hre vypni vyhlazovani (MSAA).'
    Write-Host 'CZ: Ve hre: Alt+N = menu modu, Alt+F8 = mod zap/vyp, F4 = usporny rezim (experimentalni).'
} else {
    Write-Host 'Done. bms-dlss is uninstalled. The bms-dlss folder next to bms.exe can be deleted.'
    Write-Host 'CZ: Hotovo, mod je odinstalovany. Slozku bms-dlss vedle bms.exe muzes smazat.'
}
