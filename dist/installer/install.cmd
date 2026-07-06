@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%install.ps1" -Payload "%SCRIPT_DIR%AnvilPlayerPayload.zip"
exit /b %ERRORLEVEL%
