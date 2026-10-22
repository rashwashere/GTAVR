@echo off
rem Builds the ReShade VR-loop reproduction into .\bin:
rem   openvr_api.dll        stub OpenVR (ReShade hooks it by name, builds its VR runtime)
rem   vrlooptest.exe        without the fix: OpenVR loaded as openvr_api.dll
rem   vrlooptest_fixed.exe  with OpenVRPrivateLoad.cpp: OpenVR delay-loaded under a private name
rem run.ps1 assembles a test folder with ReShade + add-ons and runs one of them.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
if not exist bin mkdir bin
set SRC=..\..\OVRInject
set TP=..\..\ThirdParty
set CFLAGS=/nologo /EHsc /MD /O2 /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /I"%TP%\openvr\headers"
set COMMON=vrlooptest.cpp "%SRC%\Log.cpp"

cl /nologo /LD /MD /O2 stub_openvr_api.cpp /Fe:bin\openvr_api.dll /Fo:bin\ /link /IMPLIB:bin\openvr_api.lib
if errorlevel 1 ( echo STUB BUILD FAILED & exit /b 1 )

cl %CFLAGS% %COMMON% /Fo:bin\ /Fe:bin\vrlooptest.exe bin\openvr_api.lib
if errorlevel 1 ( echo HARNESS BUILD FAILED & exit /b 1 )

cl %CFLAGS% %COMMON% "%SRC%\OpenVRPrivateLoad.cpp" /Fo:bin\ /Fe:bin\vrlooptest_fixed.exe bin\openvr_api.lib /link /DELAYLOAD:openvr_api.dll
if errorlevel 1 ( echo FIXED HARNESS BUILD FAILED & exit /b 1 )

echo BUILD OK
