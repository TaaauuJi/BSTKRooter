@echo off
setlocal

:: ─── Setup ───────────────────────────────────────────────────────────────
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set ROOT=%~dp0
set SRC=%ROOT%src
set IMGUI=%ROOT%imgui
set EXT4=%ROOT%..\ext4handle
set BUILD=%ROOT%build

:: Ensure build dir exists
if not exist "%BUILD%" mkdir "%BUILD%"

:: ─── Resource compilation ────────────────────────────────────────────────
echo [1/3] Compiling resources...
rc /nologo /I"%SRC%" /fo "%BUILD%\resources.res" "%SRC%\resources.rc"
if %errorlevel% neq 0 (
    echo [Build] Resource compilation FAILED.
    exit /b 1
)

:: ─── Source list ─────────────────────────────────────────────────────────
set SOURCES=^
    "%SRC%\main.cpp" ^
    "%SRC%\RootTool.cpp" ^
    "%EXT4%\src\VHDManager.cpp" ^
    "%IMGUI%\imgui.cpp" ^
    "%IMGUI%\imgui_draw.cpp" ^
    "%IMGUI%\imgui_tables.cpp" ^
    "%IMGUI%\imgui_widgets.cpp" ^
    "%IMGUI%\backends\imgui_impl_win32.cpp" ^
    "%IMGUI%\backends\imgui_impl_dx11.cpp"

:: ─── Include paths ──────────────────────────────────────────────────────
set INCLUDES=^
    /I"%SRC%" ^
    /I"%IMGUI%" ^
    /I"%IMGUI%\backends" ^
    /I"%EXT4%\src" ^
    /I"%EXT4%\external\lwext4\include" ^
    /I"%EXT4%\external\lwext4\include\generated"

:: ─── Libraries ──────────────────────────────────────────────────────────
set LIBS=d3d11.lib dxgi.lib advapi32.lib shell32.lib "%EXT4%\build\Release\lwext4.lib"

:: ─── Compile & Link ─────────────────────────────────────────────────────
echo [2/3] Compiling...
cl /nologo /W3 /O2 /MD /EHsc /std:c++17 ^
    %INCLUDES% %SOURCES% "%BUILD%\resources.res" ^
    /Fo"%BUILD%\\" ^
    /Fe"%BUILD%\BstkRooter.exe" ^
    /Fd"%BUILD%\vc140.pdb" ^
    /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup %LIBS%

if %errorlevel% neq 0 (
    echo [Build] FAILED.
    exit /b 1
)

echo [3/3] Done.
echo.
echo   Output: %BUILD%\BstkRooter.exe
