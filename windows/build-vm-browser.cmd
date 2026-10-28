@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
call :build >build\vm-browser-build.log 2>&1
if errorlevel 1 goto failed
type build\vm-browser-build.log
echo Build completed: build\vm-browser\WindowsStreamVMSetup.exe
exit /b 0

:failed
type build\vm-browser-build.log
echo Build failed. See build\vm-browser-build.log.
exit /b 1

:build
call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist build\vm-browser mkdir build\vm-browser
if not exist build\vm-browser-obj mkdir build\vm-browser-obj
cl /nologo /std:c++20 /O2 /EHsc /W4 /utf-8 /MT /DUNICODE /D_UNICODE /Fo:build\vm-browser-obj\ vm_browser\main.cpp src\common.cpp src\proxy\system_settings.cpp /Fe:build\vm-browser\WindowsStreamVMSetup.exe ^
   /link ws2_32.lib wininet.lib user32.lib shell32.lib ole32.lib advapi32.lib
if errorlevel 1 exit /b 1
copy /y config\vm-browser.ini.example build\vm-browser\client.ini >nul
if errorlevel 1 exit /b 1
exit /b 0
