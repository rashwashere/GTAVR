@echo off
rem Builds and runs the PoseTiming offline checks. Exit code = failed checks.
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist bin mkdir bin
cl /nologo /O2 /EHsc /W3 /Fo:bin\ /Fe:bin\posetimingtest.exe posetimingtest.cpp >bin\build.log || (type bin\build.log & exit /b 1)
bin\posetimingtest.exe
