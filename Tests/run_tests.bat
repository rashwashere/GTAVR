@echo off
rem Offline test harness for the stabilizer (parser + math + filter sim).
rem Textually includes PoseStabilizer.cpp, so no DLL or game needed.
rem Requires VS2022 Community (adjust the vcvars path if yours differs).

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

cd /d "%~dp0"
set SRC=..\OVRInject

cl /nologo /EHsc /MD /I"%SRC%" bindtest.cpp "%SRC%\Log.cpp" "%SRC%\Overlay.cpp" "%SRC%\Vive\Math\Matrices.cpp" /Fe:bindtest.exe user32.lib gdi32.lib d3d11.lib d3dcompiler.lib
if errorlevel 1 (
  echo BUILD FAILED
  pause
  exit /b 1
)

bindtest.exe
pause
