@echo off
rem bms-dlss: installs the mod into Black Mesa (the game must be closed). See INSTALL.txt.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0bms-dlss-setup.ps1" install %*
echo.
pause
