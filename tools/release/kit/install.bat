@echo off
rem bms-dlss: installs the mod into Black Mesa (the game must be closed). See INSTALL.txt.
if not exist "%~dp0bms-dlss-setup.ps1" (
  echo Extract the whole zip first ^(into the Black Mesa folder^), then run bms-dlss\install.bat.
  echo Nejdriv rozbal cely zip ^(do slozky Black Mesa^), pak spust bms-dlss\install.bat.
  echo.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0bms-dlss-setup.ps1" install %*
echo.
pause
