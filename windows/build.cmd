@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto failed
if not exist build\controller mkdir build\controller
if not exist build\host mkdir build\host
if not exist build\client_obj mkdir build\client_obj
if not exist build\host_obj mkdir build\host_obj

rc /nologo /i resources /fo build\client_obj\app.res resources\controller.rc
if errorlevel 1 goto failed
if /i "%~1"=="--controller-only" goto build_controller
rc /nologo /i resources /fo build\host_obj\app.res resources\host.rc
if errorlevel 1 goto failed

:build_controller
cl /nologo /std:c++20 /O2 /EHsc /W4 /utf-8 /MT /DUNICODE /D_UNICODE /Fo:build\client_obj\ src\*.cpp src\updates\*.cpp src\diagnostics\*.cpp src\proxy\*.cpp src\tunnel\*.cpp src\ping\*.cpp src\runtime\*.cpp src\locate\*.cpp shared\*.cpp shared\proxy\*.cpp /Fe:build\controller\WindowsStream.exe ^
   /link build\client_obj\app.res /SUBSYSTEM:WINDOWS /MANIFESTUAC:"level='asInvoker' uiAccess='false'" ^
   d3d11.lib dxgi.lib mfplat.lib mfuuid.lib wmcodecdspuuid.lib strmiids.lib ^
   d3dcompiler.lib ws2_32.lib iphlpapi.lib bcrypt.lib user32.lib gdi32.lib ole32.lib oleaut32.lib imm32.lib shell32.lib wininet.lib advapi32.lib
if errorlevel 1 goto failed
if /i "%~1"=="--controller-only" goto copy_controller_tools

cl /nologo /std:c++20 /O2 /EHsc /W4 /utf-8 /MT /DUNICODE /D_UNICODE /Fo:build\host_obj\ host\*.cpp host\update\*.cpp host\runtime\*.cpp host\proxy\*.cpp host\locate\*.cpp shared\*.cpp shared\proxy\*.cpp src\common.cpp /Fe:build\host\WindowsStreamHost.exe ^
   /link build\host_obj\app.res /SUBSYSTEM:WINDOWS /MANIFESTUAC:"level='asInvoker' uiAccess='false'" ^
   d3d11.lib dxgi.lib mfplat.lib mfuuid.lib wmcodecdspuuid.lib strmiids.lib winmm.lib winhttp.lib ^
   ws2_32.lib iphlpapi.lib bcrypt.lib user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib advapi32.lib
if errorlevel 1 goto failed

:copy_controller_tools
copy /y tools\setup-link.cmd build\controller\setup-link.cmd >nul
if errorlevel 1 goto failed
copy /y tools\setup-link.ps1 build\controller\setup-link.ps1 >nul
if errorlevel 1 goto failed
copy /y tools\setup-vm-proxy.cmd build\controller\setup-vm-proxy.cmd >nul
if errorlevel 1 goto failed
copy /y tools\setup-vm-proxy.ps1 build\controller\setup-vm-proxy.ps1 >nul
if errorlevel 1 goto failed
if not exist build\controller\client.ini copy /y config\client.ini build\controller\client.ini >nul
if errorlevel 1 goto failed
if not exist build\host\host.ini copy /y config\host.ini build\host\host.ini >nul
if errorlevel 1 goto failed
echo Build completed.
echo Controller: %~dp0build\controller
if /i "%~1"=="--controller-only" goto completed
echo Host:       %~dp0build\host
:completed
if /i "%~1"=="--no-pause" exit /b 0
if /i "%~2"=="--no-pause" exit /b 0
start "" "%~dp0build"
pause
exit /b 0

:failed
echo Build failed. Review the error above.
if /i "%~1"=="--no-pause" exit /b 1
if /i "%~2"=="--no-pause" exit /b 1
pause
exit /b 1
