@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0fix-access.ps1"
if errorlevel 1 echo PostgreSQL access repair failed.
pause
