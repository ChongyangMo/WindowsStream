@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
call :build >build\server-host-build.log 2>&1
if errorlevel 1 goto failed
type build\server-host-build.log
echo Build completed: build\server-host\WindowsStreamHost.exe
if /i "%~1"=="--no-pause" exit /b 0
pause
exit /b 0
:failed
type build\server-host-build.log
if /i "%~1"=="--no-pause" exit /b 1
pause
exit /b 1
:build
call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist build\server-host mkdir build\server-host
if not exist build\server-host-obj mkdir build\server-host-obj
rc /nologo /i resources /fo build\server-host-obj\app.res resources\host.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /W4 /utf-8 /MT /DUNICODE /D_UNICODE /Fo:build\server-host-obj\ host\*.cpp host\update\*.cpp host\runtime\*.cpp host\proxy\*.cpp host\locate\*.cpp host\service\*.cpp shared\*.cpp shared\proxy\*.cpp src\common.cpp /Fe:build\server-host\WindowsStreamHost.exe ^
 /link build\server-host-obj\app.res /SUBSYSTEM:WINDOWS /MANIFESTUAC:"level='asInvoker' uiAccess='false'" ^
 d3d11.lib dxgi.lib mfplat.lib mfuuid.lib wmcodecdspuuid.lib strmiids.lib winmm.lib winhttp.lib ^
 ws2_32.lib iphlpapi.lib bcrypt.lib user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib advapi32.lib wtsapi32.lib
if errorlevel 1 exit /b 1
exit /b 0
