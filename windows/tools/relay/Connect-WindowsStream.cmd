@echo off
powershell.exe -NoProfile -NonInteractive -STA -ExecutionPolicy Bypass -File "%~dp0connect-universal.ps1" %*
