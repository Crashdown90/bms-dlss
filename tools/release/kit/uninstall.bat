@echo off
rem bms-dlss: restores the original DXVK of Black Mesa (the game must be closed). See INSTALL.txt.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0bms-dlss-setup.ps1" uninstall %*
echo.
pause
