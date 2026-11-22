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
    # 2) Steam libraries (registry + libraryfolders.vdf)
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
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libs += ($m.Groups[1].Value -replace '\\\\', '\')
        }
    }
    foreach ($lib in ($libs | Select-Object -Unique)) {
        $manifest = Join-Path $lib "steamapps\appmanifest_$appId.acf"
        if (-not (Test-Path $manifest)) { continue }
        $dir = 'Black Mesa'
        $m = Select-String -Path $manifest -Pattern '"installdir"\s+"([^"]+)"' | Select-Object -First 1
        if ($m) { $dir = $m.Matches[0].Groups[1].Value }
        $g = Join-Path $lib "steamapps\common\$dir"
        if (Test-GameDir $g) { return $g }
    }
    return $null
}

if (-not $GameDir) { $GameDir = Find-GameDir }
if (-not (Test-GameDir $GameDir)) {
    throw "Black Mesa not found. Extract the package into the game folder (Steam > Black Mesa > Manage > Browse local files) or run: bms-dlss-setup.ps1 $Action -GameDir `"<game folder>`""
}
Write-Host "Black Mesa: $GameDir"

$dxvk = Join-Path $GameDir 'bin\thirdparty\dxvk-windows-x86'
if ($Action -eq 'install' -and -not (Test-Path (Join-Path $dxvk 'd3d9.dll'))) {
    throw ("The game has no DXVK folder ($dxvk). bms-dlss needs the current Steam version of Black Mesa for Windows " +
           "with the launch option 'DXVK'. Update the game or verify its files in Steam and try again.")
}

$script = Join-Path $PSScriptRoot "$Action.ps1"
& $script -GameDir $GameDir

if ($Action -eq 'install') {
    Write-Host ''
    Write-Host 'Done. Start the game with the Steam launch option "Play DXVK" (without it the mod is not loaded).'
    Write-Host 'In game: Alt+N opens the bms-dlss menu.'
    Write-Host 'CZ: Hotovo. Hru spoustej volbou Steamu "Hrat DXVK" (bez ni se mod nenacte). Ve hre: Alt+N = menu modu.'
}
