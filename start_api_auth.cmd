@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start_api.ps1" -Mode auth %*
set "BREEZE_EXIT_CODE=%ERRORLEVEL%"
if not "%BREEZE_EXIT_CODE%"=="0" if not defined BREEZE_NO_PAUSE pause
exit /b %BREEZE_EXIT_CODE%
