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
rem  This machine has no .NET SDK (C:\Program Files\dotnet contains only
rem  host/shared, no sdk), so "dotnet build" is unavailable. Instead we invoke
rem  the Roslyn compiler shipped with VS BuildTools and reference the
rem  assemblies from the installed .NET Framework runtime.
rem
rem  NOTE: comments here are kept ASCII on purpose. cmd.exe reads .bat files
rem  using the OEM code page, so non-ASCII comments can be mis-parsed into
rem  bogus commands. Also note the goto-based flow instead of "if (...)"
rem  blocks: expanding a variable that contains "(x86)" inside parentheses
rem  breaks the parser.
rem
rem  Usage: double-click it, or run  client\build.bat
rem ---------------------------------------------------------------------------

set "CSC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe"
if not exist "%CSC%" goto no_csc

set "HERE=%~dp0"
for %%I in ("%HERE%..") do set "ROOT=%%~fI"
set "OUT=%ROOT%\ShittimLogonUpdaterNet.exe"

"%CSC%" /nologo /target:winexe /platform:x64 /langversion:7.3 /codepage:65001 ^
    /win32manifest:"%HERE%app.manifest" ^
    /out:"%OUT%" ^
    /reference:System.dll ^
    /reference:System.Core.dll ^
    /reference:System.Drawing.dll ^
    /reference:System.Windows.Forms.dll ^
    /reference:System.IO.Compression.dll ^
    /reference:System.IO.Compression.FileSystem.dll ^
    "%HERE%AssemblyInfo.cs" ^
    "%HERE%Crc32.cs" ^
    "%HERE%Updater.cs" ^
    "%HERE%MainForm.cs" ^
    "%HERE%Program.cs"

if errorlevel 1 goto failed

echo.
echo [OK] %OUT%
exit /b 0

:no_csc
echo [ERROR] Roslyn compiler not found:
echo         %CSC%
exit /b 1

:failed
echo.
echo [ERROR] Compilation failed
exit /b 1
