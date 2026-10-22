@echo off
setlocal EnableExtensions EnableDelayedExpansion
title VR Switch - GTA:Vive

rem ============================================================
rem  Optional. Switches GTA:Vive on or off for the game this file
rem  belongs to: it can sit in the game folder (next to GTA5.exe or
rem  GTA5_Enhanced.exe) or in a folder inside it, such as Optional.
rem    1 = enable, 2 = disable, 3 = exit.
rem
rem  GTAVive is found by CONTENT, not by name: the .asi / .as file
rem  in the game folder that links BOTH OVRInject.dll and
rem  ScriptHookV.dll (so a renamed GTAVive.asi is found too).
rem    enabled = *.asi     disabled = *.as     *.bak is never touched
rem
rem  With GTAVive off, OVRInject.dll never loads, so the game does
rem  not start SteamVR.
rem
rem  No menu, for a shortcut:  "VR Switch.bat" /do enable|disable
rem ============================================================

set "GP=%~dp0"
set "GP=!GP:~0,-1!"
if not exist "!GP!\GTA5.exe" if not exist "!GP!\GTA5_Enhanced.exe" for %%P in ("!GP!\..") do set "GP=%%~fP"

if not exist "!GP!\GTA5.exe" if not exist "!GP!\GTA5_Enhanced.exe" (
  echo.
  echo  Put this file in the GTA V game folder ^(next to GTA5.exe or GTA5_Enhanced.exe^)
  echo  or in a folder inside it, and run it from there.
  echo.
  if /i not "%~1"=="/do" pause
  exit /b 1
)

set "NLOG=0"

if /i "%~1"=="/do" (
  if /i "%~2"=="enable"  call :apply on
  if /i "%~2"=="disable" call :apply off
  for /l %%i in (1,1,!NLOG!) do echo !LOG%%i!
  call :scan
  echo GTAVive=!VIVE_STATE! [!VIVE_FILES!]
  exit /b 0
)

for /f %%a in ('echo prompt $E^| cmd') do set "ESC=%%a"
set "C_GREEN=%ESC%[97;42m"
set "C_RED=%ESC%[97;41m"
set "C_YELLOW=%ESC%[30;43m"
set "C_DIM=%ESC%[90m"
set "C_BOLD=%ESC%[97m"
set "C_OFF=%ESC%[0m"

:main
cls
echo.
echo  %C_BOLD%VR ENABLER%C_OFF%   GTA:Vive on / off
echo  %C_DIM%GREEN = VR on   RED = VR off   YELLOW = mixed / not found%C_OFF%
echo.
call :show
if !NLOG! gtr 0 (
  echo  %C_DIM%Last change:%C_OFF%
  for /l %%i in (1,1,!NLOG!) do echo !LOG%%i!
  echo.
)
echo   %C_BOLD%1%C_OFF%  Enable VR
echo   %C_BOLD%2%C_OFF%  Disable VR
echo   %C_BOLD%3%C_OFF%  Exit
echo.
choice /c 123 /n /m "  Choose 1-3: "
if errorlevel 3 goto :eof
if errorlevel 2 (set "WANT=off") else (set "WANT=on")

set "NLOG=0"
call :apply !WANT!
goto :main


rem ------------------------------------------------------------
rem :say text   adds a line to the "Last change" list
rem ------------------------------------------------------------
:say
set /a NLOG+=1
set "LOG!NLOG!=%~1"
exit /b

rem ------------------------------------------------------------
rem :show   prints the colored status block
rem ------------------------------------------------------------
:show
call :scan
set "COL=%C_YELLOW%" & set "HEAD=NOT CLEAR"
if "!VIVE_STATE!"=="on"      ( set "COL=%C_GREEN%" & set "HEAD=VR ON" )
if "!VIVE_STATE!"=="off"     ( set "COL=%C_RED%"   & set "HEAD=VR OFF" )
if "!VIVE_STATE!"=="missing" set "HEAD=GTAVIVE NOT FOUND"
echo  !COL!  !HEAD!  %C_OFF%
echo     GTAVive : !VIVE_STATE!   !VIVE_FILES!
if "!RUNNING!"=="1" echo     %C_YELLOW% GAME IS RUNNING - close it before switching %C_OFF%
echo.
exit /b

rem ------------------------------------------------------------
rem :isvive file   -> ISVIVE=1 when the file is a GTAVive script
rem ------------------------------------------------------------
:isvive
set "ISVIVE=0"
if /i "%~x1"==".asi" set "ISVIVE=1"
if /i "%~x1"==".as"  set "ISVIVE=1"
if "!ISVIVE!"=="1" ( findstr /m /i /c:"OVRInject.dll" "%~1" >nul 2>&1 || set "ISVIVE=0" )
if "!ISVIVE!"=="1" ( findstr /m /i /c:"ScriptHookV.dll" "%~1" >nul 2>&1 || set "ISVIVE=0" )
exit /b

rem ------------------------------------------------------------
rem :scan   -> VIVE_STATE (on, off, mixed, missing), VIVE_FILES, RUNNING
rem ------------------------------------------------------------
:scan
set "VIVE_ON=0" & set "VIVE_OFF=0" & set "VIVE_FILES=" & set "RUNNING=0"

for %%E in (GTA5.exe GTA5_Enhanced.exe) do (
  "%SystemRoot%\System32\tasklist.exe" /fi "imagename eq %%E" 2>nul | "%SystemRoot%\System32\find.exe" /i "%%E" >nul && set "RUNNING=1"
)

for %%F in ("!GP!\*.as*") do (
  call :isvive "%%F"
  if "!ISVIVE!"=="1" (
    if /i "%%~xF"==".asi" ( set /a VIVE_ON+=1 ) else ( set /a VIVE_OFF+=1 )
    set "VIVE_FILES=!VIVE_FILES! %%~nxF"
  )
)

set "VIVE_STATE=mixed"
if !VIVE_ON! gtr 0 if !VIVE_OFF! equ 0 set "VIVE_STATE=on"
if !VIVE_ON! equ 0 if !VIVE_OFF! gtr 0 set "VIVE_STATE=off"
if !VIVE_ON! equ 0 if !VIVE_OFF! equ 0 ( set "VIVE_STATE=missing" & set "VIVE_FILES= (no GTAVive file found)" )
exit /b

rem ------------------------------------------------------------
rem :apply on|off   renames the GTAVive file(s)
rem ------------------------------------------------------------
:apply
call :scan
set "LOGSTART=!NLOG!"
if "!RUNNING!"=="1" ( call :say "    game is running - close it first, nothing changed" & exit /b )
if "!VIVE_STATE!"=="missing" ( call :say "    no GTAVive file found in this folder" & exit /b )

for %%F in ("!GP!\*.as*") do (
  call :isvive "%%F"
  if "!ISVIVE!"=="1" (
    if "%1"=="on"  if /i "%%~xF"==".as"  call :ren "%%F" "%%~nF.asi"
    if "%1"=="off" if /i "%%~xF"==".asi" call :ren "%%F" "%%~nF.as"
  )
)

if !NLOG! equ !LOGSTART! call :say "    VR was already %1 - nothing to change"
exit /b

:ren
if exist "%~dp1%~2" ( call :say "    SKIPPED %~nx1 - %~2 already exists" & exit /b )
ren "%~1" "%~2" && ( call :say "    %~nx1  ->  %~2" ) || ( call :say "    FAILED to rename %~nx1 - file in use?" )
exit /b
