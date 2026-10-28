@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
call :build >build\controller-build.log 2>&1
if errorlevel 1 goto failed
type build\controller-build.log
echo Build completed: build\controller\WindowsStream.next.exe
if /i "%~1"=="--no-pause" exit /b 0
pause
exit /b 0

:failed
type build\controller-build.log
echo Build failed. See build\controller-build.log.
if /i "%~1"=="--no-pause" exit /b 1
pause
exit /b 1

:build
hostname
call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist build\controller mkdir build\controller
if not exist build\client_obj mkdir build\client_obj
rc /nologo /i resources /fo build\client_obj\app.res resources\controller.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /W4 /utf-8 /MT /DUNICODE /D_UNICODE /Fo:build\client_obj\ src\*.cpp src\updates\*.cpp src\diagnostics\*.cpp src\proxy\*.cpp src\tunnel\*.cpp src\ping\*.cpp src\runtime\*.cpp src\runtime\workspace\*.cpp src\locate\*.cpp shared\*.cpp shared\proxy\*.cpp /Fe:build\controller\WindowsStream.next.exe ^
   /link build\client_obj\app.res /SUBSYSTEM:WINDOWS /MANIFESTUAC:"level='asInvoker' uiAccess='false'" ^
   d3d11.lib dxgi.lib mfplat.lib mfuuid.lib wmcodecdspuuid.lib strmiids.lib ^
   d3dcompiler.lib ws2_32.lib iphlpapi.lib bcrypt.lib user32.lib gdi32.lib ole32.lib oleaut32.lib imm32.lib shell32.lib wininet.lib advapi32.lib comctl32.lib
if errorlevel 1 exit /b 1
copy /y tools\setup-vm-proxy.cmd build\controller\setup-vm-proxy.cmd >nul
if errorlevel 1 exit /b 1
copy /y tools\setup-vm-proxy.ps1 build\controller\setup-vm-proxy.ps1 >nul
if errorlevel 1 exit /b 1
exit /b 0
