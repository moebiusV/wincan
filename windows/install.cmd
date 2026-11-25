@echo off
rem install.cmd -- runs install.ps1 whatever the PowerShell execution policy is, and keeps the window open.
rem   install.cmd -Server https://HOST:8766 -ServerPin SHA256:...   (the options of install.ps1)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
