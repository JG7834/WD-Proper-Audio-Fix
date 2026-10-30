@echo off
rem WDAudioFix setup
rem   setup.bat         - fetch MinHook (first run), then open the solution in Visual Studio
rem   setup.bat build   - fetch MinHook (first run), then build Release without opening VS
setlocal EnableExtensions
cd /d "%~dp0"

rem ---------------------------------------------------------------- MinHook
if exist "minhook\src\hook.c" goto :gamedir
echo Downloading MinHook...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;" ^
  "Invoke-WebRequest 'https://github.com/TsudaKageyu/minhook/archive/refs/heads/master.zip' -OutFile 'minhook.zip';" ^
  "Expand-Archive 'minhook.zip' -DestinationPath '.' -Force;" ^
  "if (Test-Path 'minhook') { Remove-Item 'minhook' -Recurse -Force };" ^
  "Rename-Item 'minhook-master' 'minhook';" ^
  "Remove-Item 'minhook.zip'"
if errorlevel 1 goto :mhfail
if not exist "minhook\src\hook.c" goto :mhfail

rem ---------------------------------------------------------------- game folder (asked once)
:gamedir
if exist "local.props" goto :vs
echo.
echo Optional: the Watch Dogs "bin" folder, where Watch_Dogs.exe is.
echo If set, every build copies WDAudioFix.asi there automatically.
echo Press Enter to skip. Delete local.props to be asked again.
set "WDBIN="
set /p "WDBIN=Game bin folder: "
if not defined WDBIN goto :vs
set "WDBIN=%WDBIN:"=%"
if "%WDBIN:~-1%"=="\" set "WDBIN=%WDBIN:~0,-1%"
if not exist "%WDBIN%\Watch_Dogs.exe" echo Note: Watch_Dogs.exe not found in that folder - using it anyway.
>  local.props echo ^<?xml version="1.0" encoding="utf-8"?^>
>> local.props echo ^<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"^>
>> local.props echo   ^<PropertyGroup^>^<GameBinDir^>%WDBIN%^</GameBinDir^>^</PropertyGroup^>
>> local.props echo ^</Project^>

rem ---------------------------------------------------------------- open or build
:vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if /i "%~1"=="build" goto :build

set "DEVENV="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property productPath`) do set "DEVENV=%%i"
if defined DEVENV (
  start "" "%DEVENV%" "%~dp0WDAudioFix.sln"
) else (
  start "" "%~dp0WDAudioFix.sln"
)
exit /b 0

:build
set "MSBUILD="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD goto :nomsbuild
"%MSBUILD%" "%~dp0WDAudioFix.sln" /p:Configuration=Release /p:Platform=x64 /m /v:minimal
if errorlevel 1 goto :buildfail
echo.
echo Built: %~dp0bin\Release\WDAudioFix.asi
pause
exit /b 0

:mhfail
echo.
echo MinHook download failed. Download https://github.com/TsudaKageyu/minhook manually
echo and extract it so that minhook\src\hook.c exists next to this script.
pause
exit /b 1

:nomsbuild
echo.
echo Visual Studio with "Desktop development with C++" was not found.
pause
exit /b 1

:buildfail
echo.
echo Build failed - see the errors above.
pause
exit /b 1
