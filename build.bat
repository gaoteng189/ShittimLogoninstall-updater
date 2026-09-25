@echo off
setlocal enabledelayedexpansion

rem ==========================================================================
rem  ShittimLogon Updater - build script (MSVC x64)
rem ==========================================================================

set "ROOT=%~dp0"
set "OUTDIR=%ROOT%build"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Please install Visual Studio 2022 Build Tools
    echo         with the "Desktop development with C++" workload.
    exit /b 1
)

set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%i"
)

if "%VSINSTALL%"=="" (
    echo [ERROR] No Visual Studio installation with C++ build tools was found.
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] Failed to initialize the MSVC environment.
    exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo [INFO] Compiling with MSVC (x64, release)...

pushd "%ROOT%"
cl /nologo /std:c++17 /utf-8 /EHsc /W3 /O2 /MT /DNDEBUG ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /I include ^
   /Fo"%OUTDIR%\\" /Fe"%OUTDIR%\ShittimLogonUpdater.exe" ^
   src\main.cpp src\http_client.cpp src\inflate.cpp src\zip_extractor.cpp ^
   src\process_launcher.cpp src\sha256.cpp src\logger.cpp src\util.cpp ^
   /link /SUBSYSTEM:CONSOLE winhttp.lib bcrypt.lib shell32.lib ole32.lib advapi32.lib
set "RESULT=!errorlevel!"
popd

if not "!RESULT!"=="0" (
    echo.
    echo [ERROR] Build failed with exit code !RESULT!.
    exit /b !RESULT!
)

echo.
echo [OK] Build succeeded: %OUTDIR%\ShittimLogonUpdater.exe
exit /b 0
