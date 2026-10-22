@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
if not exist bin mkdir bin
if not exist out mkdir out
cl /nologo /O2 /EHsc /MD /Fo"bin\\" /Fe"bin\posecadence.exe" posecadence.cpp "..\..\ThirdParty\openvr_api.lib" winmm.lib || exit /b 1
echo Built bin\posecadence.exe
