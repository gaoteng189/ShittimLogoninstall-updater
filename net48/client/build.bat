@echo off
setlocal

rem ---------------------------------------------------------------------------
rem  Build the .NET Framework 4.8 + WinForms client.
rem
rem  The result is written to net48\dist\ShittimLogonUpdaterNet.exe -- inside the
rem  net48 tree, next to its source, so the two .NET runtimes never mix. The
rem  "Net" suffix keeps it clear of the C++ console build, which owns the plain
rem  ShittimLogonUpdater.exe name.
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
rem  Usage: double-click it, or run  net48\client\build.bat
rem ---------------------------------------------------------------------------

set "HERE=%~dp0"
rem RUNTIME is the runtime root (this project's parent: net48).
for %%I in ("%HERE%..") do set "RUNTIME=%%~fI"
set "OUT=%RUNTIME%\dist\ShittimLogonUpdaterNet.exe"

where dotnet >nul 2>nul
if errorlevel 1 goto no_dotnet

rem Force English output: only the messages change, never the build result.
set "DOTNET_CLI_UI_LANGUAGE=en"

dotnet build "%HERE%ShittimLogonUpdaterNet48.csproj" -c Release --nologo -v minimal
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
