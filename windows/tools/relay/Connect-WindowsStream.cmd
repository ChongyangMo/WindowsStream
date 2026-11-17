@echo off
powershell.exe -NoProfile -NonInteractive -STA -ExecutionPolicy Bypass -File "%~dp0.runtime\connect-universal.ps1" %*
