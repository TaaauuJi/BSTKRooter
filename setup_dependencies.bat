@echo off
setlocal
echo =========================================================
echo BSTK Rooter - Dependency Setup Script
echo =========================================================
echo.

:menu
echo Select an option:
echo 1. Check all dependencies
echo 2. Download / Update ImGui
echo 3. Download / Update lwext4 (EXT4 driver)
echo 4. Exit
echo.
set /p choice="Enter choice [1-4]: "

if "%choice%"=="1" goto check
if "%choice%"=="2" goto download_imgui
if "%choice%"=="3" goto download_lwext4
if "%choice%"=="4" exit /b 0

goto menu

:check
echo.
set MISSING=0

if not exist "%~dp0lwext4" (
    echo [FAIL] 'lwext4' is missing!
    echo Please download it via option 3 in this script.
    set MISSING=1
) else (
    echo [OK] FOUND: lwext4
)

if not exist "%~dp0imgui" (
    echo [FAIL] 'imgui' is missing!
    echo Please download it via option 2 in this script.
    set MISSING=1
) else (
    echo [OK] FOUND: imgui
)

echo.
if "%MISSING%"=="1" (
    echo Some dependencies are missing. Please resolve them before building.
) else (
    echo All requirements are satisfied! You can now cleanly run build.bat.
)
echo.
pause
cls
goto menu

:download_imgui
echo.
echo [INFO] Fetching Dear ImGui from GitHub...
where git >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Git is not installed or not found in PATH!
    pause
    cls
    goto menu
)

if exist "%~dp0imgui" (
    echo [INFO] ImGui directory already exists. Updating...
    cd "%~dp0imgui"
    git pull
    cd "%~dp0"
) else (
    git clone -b docking https://github.com/ocornut/imgui.git "%~dp0imgui"
)
echo.
echo [OK] ImGui setup completed.
echo.
pause
cls
goto menu

:download_lwext4
echo.
echo [INFO] Fetching lwext4 from GitHub...
where git >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Git is not installed or not found in PATH!
    pause
    cls
    goto menu
)

if exist "%~dp0lwext4" (
    echo [INFO] lwext4 directory already exists. Updating...
    cd "%~dp0lwext4"
    git pull
    cd "%~dp0"
) else (
    git clone https://github.com/gkostka/lwext4.git "%~dp0lwext4"
)

echo [INFO] Ensuring ext4_config.h configuration exists...
if not exist "%~dp0lwext4\include\generated" mkdir "%~dp0lwext4\include\generated"
(
echo #ifndef EXT4_CONFIG_H_
echo #define EXT4_CONFIG_H_
echo #include ^<stdint.h^>
echo #include ^<stddef.h^>
echo #include ^<errno.h^>
echo #define CONFIG_DEBUG_PRINTF 0
echo #define CONFIG_DEBUG_ASSERT 1
echo #define CONFIG_BLOCK_DEV_CACHE_SIZE 16
echo #define CONFIG_EXTENT_ENABLE 1
echo #define CONFIG_XATTR_ENABLE 1
echo #define CONFIG_JOURNALING_ENABLE 0
echo #define CONFIG_HAVE_OWN_ERRNO 0
echo #define CONFIG_HAVE_OWN_ASSERT 0
echo #ifndef EOK
echo #define EOK 0
echo #endif
echo #endif
) > "%~dp0lwext4\include\generated\ext4_config.h"

echo.
echo [OK] lwext4 setup completed.
echo.
pause
cls
goto menu
