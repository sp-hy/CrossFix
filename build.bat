@echo off
setlocal
cd /d "%~dp0"

set "CONFIG=Release - Proxy"
set "PLATFORM=Win32"

echo Build started at %time%...
echo.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Error: vswhere.exe not found. Is Visual Studio installed?
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo Error: Could not find Visual Studio installation.
    exit /b 1
)

set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    echo Error: vcvarsall.bat not found at %VCVARS%
    exit /b 1
)

call "%VCVARS%" x86
if errorlevel 1 (
    echo Error: Failed to initialize Visual Studio environment.
    exit /b 1
)

msbuild crossfix.sln /p:Configuration="%CONFIG%" /p:Platform=%PLATFORM% /nologo /v:minimal
set BUILD_EXIT=%errorlevel%

echo.
if %BUILD_EXIT% equ 0 (
    echo ========== Build: 1 succeeded, 0 failed ==========
) else (
    echo ========== Build failed ==========
)

echo Build completed at %time%
endlocal
exit /b %BUILD_EXIT%
