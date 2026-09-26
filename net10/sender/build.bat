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
rem  Usage: double-click it, or run  net10\sender\build.bat
rem ---------------------------------------------------------------------------

set "HERE=%~dp0"
rem RUNTIME is the runtime root (this project's parent: net10). Everything lands
rem under its dist folder, so net10 and net48 never mix.
for %%I in ("%HERE%..") do set "RUNTIME=%%~fI"
set "OUT=%RUNTIME%\dist\sender-x64\ShittimLogonSender.exe"

where dotnet >nul 2>nul
if errorlevel 1 goto no_dotnet

rem Force English output: only the messages change, never the build result.
set "DOTNET_CLI_UI_LANGUAGE=en"

dotnet build "%HERE%ShittimLogonSender.csproj" -c Release --nologo -v minimal
if errorlevel 1 goto failed

if not exist "%OUT%" goto missing

rem  Also produce a single-file build -- see net10\client\build.bat for the
rem  full explanation of the trade-offs involved. Both the folder build and the
rem  single-file build land in net10\dist\, next to the client's output.
set "SINGLE=%RUNTIME%\dist\ShittimLogonSender-single.exe"
set "STAGE=%RUNTIME%\dist\_single-stage-sender"

if exist "%STAGE%" rmdir /s /q "%STAGE%"

dotnet publish "%HERE%ShittimLogonSender.csproj" -c Release -r win-x64 --self-contained ^
    -o "%STAGE%" ^
    -p:PublishSingleFile=true ^
    -p:IncludeNativeLibrariesForSelfExtract=true ^
    -p:PublishReadyToRun=false ^
    --nologo -v minimal
if errorlevel 1 goto failed

if not exist "%STAGE%\ShittimLogonSender.exe" goto single_missing

move /y "%STAGE%\ShittimLogonSender.exe" "%SINGLE%" >nul
if exist "%STAGE%" rmdir /s /q "%STAGE%"

echo.
echo [OK] %OUT%
echo      folder build -- copy the whole net10\dist\sender-x64\ directory
echo [OK] %SINGLE%
echo      single-file build -- copy just this one file
exit /b 0

:single_missing
echo [ERROR] Single-file publish produced no exe in %STAGE%
exit /b 1

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
