:: ============================================================
::  SuperCOM (dynamic) deploy script
::  Copies the freshly-built SuperCOM.exe into cpp/dist/ with
::  all required Qt DLLs and platform plugins via windeployqt.
:: ============================================================
@echo off
setlocal enableextensions

rem --- toolchain / Qt paths (edit here if you ever move them) ---
set "QT_DIR=D:\dev\qt\6.8.2\mingw_64"
set "MINGW_DIR=D:\dev\qt\Tools\mingw1310_64"
set "SRC_DIR=%~dp0.."
set "BUILD=%SRC_DIR%\build_mingw"
set "DIST=%SRC_DIR%\dist"

rem --- PATH: include MinGW (for windeployqt) and Qt's bin (for missing tools) ---
set "PATH=%MINGW_DIR%\bin;%QT_DIR%\bin;%PATH%"

echo == [1/4] Kill running SuperCOM.exe (if any) =========================
taskkill /F /IM SuperCOM.exe 2>nul
if errorlevel 1 (
    echo    no running instance
) else (
    echo    killed
)

if not exist "%BUILD%\SuperCOM.exe" (
    echo [ERROR] %BUILD%\SuperCOM.exe not found. Build first:
    echo    cmake -S "%SRC_DIR%" -B "%BUILD%" -G Ninja -DCMAKE_PREFIX_PATH=%QT_DIR% -DAPP_VERSION=dev
    echo    cmake --build "%BUILD%" -j
    exit /b 1
)

echo == [2/4] Backup current dist exe =================================
if exist "%DIST%\SuperCOM.exe" (
    if exist "%DIST%\_old_SuperCOM.exe" del /F /Q "%DIST%\_old_SuperCOM.exe" >nul 2>&1
    ren  "%DIST%\SuperCOM.exe" _old_SuperCOM.exe
    echo    previous exe archived as _old_SuperCOM.exe
)

echo == [3/4] Deploy DLLs + plugins into cpp\dist via windeployqt =======
"%QT_DIR%\bin\windeployqt.exe" ^
    --release ^
    --no-translations ^
    --no-system-d3d-compiler ^
    --dir "%DIST%" ^
    "%BUILD%\SuperCOM.exe"
if errorlevel 1 (
    echo [ERROR] windeployqt failed
    exit /b 1
)

echo == [4/4] Copy the freshly-built exe into cpp\dist ================
copy /Y "%BUILD%\SuperCOM.exe" "%DIST%\SuperCOM.exe" >nul
echo    deployment done.

echo.
echo Run: %DIST%\SuperCOM.exe
endlocal
exit /b 0
