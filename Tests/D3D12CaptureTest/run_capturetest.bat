@echo off
rem Offline test of the GTA V Enhanced (D3D12) capture path: fake D3D12 game
rem window + the real D3D12Capture.cpp + a D3D11 reader. No game, no headset.
rem A small window flashes colors for ~9 seconds, then PASS/FAIL is printed.

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

cd /d "%~dp0"
set SRC=..\..\OVRInject
set TP=..\..\ThirdParty

cl /nologo /EHsc /MD /O2 /DOVRINJECT_EXPORTS /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /I"%TP%\minhook\include" /I"%TP%\openvr\headers" capturetest.cpp "%SRC%\D3D12Hook\D3D12Capture.cpp" "%SRC%\Log.cpp" /Fe:capturetest.exe "%TP%\libMinHook.x64.lib"
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

"%~dp0capturetest.exe"
