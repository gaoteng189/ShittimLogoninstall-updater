@echo off
setlocal

rem ---------------------------------------------------------------------------
rem  Build the .NET Framework 4.8 + WinForms client.
rem
rem  The result is written straight to the repository root as
rem  ShittimLogonUpdaterNet.exe, so it can be double-clicked without hunting
rem  through a build folder. The "Net" suffix keeps it clear of the C++
rem  console build, which owns the plain ShittimLogonUpdater.exe name.
rem
rem  Version info (product name, copyright, file version) comes from
rem  AssemblyInfo.cs -- edit it there, not here.
rem
rem  Thin wrapper around "dotnet build": the project file carries the real
rem  settings, this script only exists so the build stays one double-click
rem  away and so a sane output language is forced (on a Chinese locale the
rem  CLI sometimes emits doubled characters).
rem
rem  Requirements:
rem    - .NET SDK                            (verified with 10.0.401)
rem    - .NET Framework 4.8 Developer Pack   (supplies the net48 reference
rem      assemblies; without it the compiler cannot find System.Windows.Forms)
rem
rem  NOTE: comments here are kept ASCII on purpose. cmd.exe reads .bat files
rem  using the OEM code page, so non-ASCII comments can be mis-parsed into
rem  bogus commands.
rem
rem  Usage: double-click it, or run  client\build.bat
rem ---------------------------------------------------------------------------

set "HERE=%~dp0"
for %%I in ("%HERE%..") do set "ROOT=%%~fI"
set "OUT=%ROOT%\ShittimLogonUpdaterNet.exe"

where dotnet >nul 2>nul
if errorlevel 1 goto no_dotnet

rem Force English output: only the messages change, never the build result.
set "DOTNET_CLI_UI_LANGUAGE=en"

dotnet build "%HERE%ShittimLogonUpdater.csproj" -c Release --nologo -v minimal
if errorlevel 1 goto failed

if not exist "%OUT%" goto missing

echo.
echo [OK] %OUT%
exit /b 0

:no_dotnet
echo [ERROR] dotnet not found on PATH.
echo         Install the .NET SDK from https://aka.ms/dotnet/download
exit /b 1

:missing
echo [ERROR] The build reported success but the expected output is missing:
echo         %OUT%
exit /b 1

:failed
echo.
echo [ERROR] Build failed
exit /b 1
