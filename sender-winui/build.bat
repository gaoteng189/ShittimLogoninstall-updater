@echo off
setlocal

rem ---------------------------------------------------------------------------
rem  Build the WinUI 3 sender (Windows App SDK + .NET 10).
rem
rem  Thin wrapper around "dotnet build": the project file carries the real
rem  settings, this script only exists so the build stays one double-click away
rem  and so a sane output language is forced (on a Chinese locale the CLI
rem  sometimes emits doubled characters).
rem
rem  Output is self-contained: dist\sender-x64\ holds the exe plus the .NET and
rem  Windows App SDK runtimes, so the whole folder can be copied to a machine
rem  with nothing installed.
rem
rem  NOTE: comments here are kept ASCII on purpose. cmd.exe reads .bat files
rem  using the OEM code page, so non-ASCII comments can be mis-parsed into
rem  bogus commands.
rem
rem  Usage: double-click it, or run  sender-winui\build.bat
rem ---------------------------------------------------------------------------

set "HERE=%~dp0"
for %%I in ("%HERE%..") do set "ROOT=%%~fI"
set "OUT=%ROOT%\dist\sender-x64\ShittimLogonSender.exe"

where dotnet >nul 2>nul
if errorlevel 1 goto no_dotnet

rem Force English output: only the messages change, never the build result.
set "DOTNET_CLI_UI_LANGUAGE=en"

dotnet build "%HERE%ShittimLogonSender.csproj" -c Release --nologo -v minimal
if errorlevel 1 goto failed

if not exist "%OUT%" goto missing

echo.
echo [OK] %OUT%
echo      (self-contained folder -- copy the whole dist\sender-x64\ directory)
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
