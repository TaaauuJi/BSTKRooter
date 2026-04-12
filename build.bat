@echo off
setlocal

:: BSTKRooter
:: Copyright (c) 2026 Taaauu
::
:: This program is free software: you can redistribute it and/or modify
:: it under the terms of the GNU General Public License as published by
:: the Free Software Foundation, either version 3 of the License, or
:: (at your option) any later version.
::
:: This program is distributed in the hope that it will be useful,
:: but WITHOUT ANY WARRANTY; without even the implied warranty of
:: MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
:: GNU General Public License for more details.
::
:: You should have received a copy of the GNU General Public License
:: along with this program.  If not, see <https://www.gnu.org/licenses/>.

:: ─── Setup ───────────────────────────────────────────────────────────────
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set ROOT=%~dp0
set SRC=%ROOT%src
set IMGUI=%ROOT%imgui
set LWEXT4=%ROOT%lwext4
set BUILD=%ROOT%build

:: Ensure build dir exists
if not exist "%BUILD%" mkdir "%BUILD%"

:: ─── Encrypt su_c resource ───────────────────────────────────────────────
echo [0/3] Encrypting su resource...
where python >nul 2>&1 && (
    python "%ROOT%scripts\encrypt_resource.py"
) || (
    echo [Build] Python not found, using existing su_c.enc if available.
)

:: ─── Resource compilation ────────────────────────────────────────────────
echo [1/3] Compiling resources...
rc /nologo /I"%SRC%" /fo "%BUILD%\resources.res" "%SRC%\resources.rc"
if %errorlevel% neq 0 (
    echo [Build] Resource compilation FAILED.
    exit /b 1
)

:: ─── Compile lwext4 library ──────────────────────────────────────────────
if not exist "%BUILD%\lwext4" mkdir "%BUILD%\lwext4"
if not exist "%BUILD%\lwext4.lib" (
    echo [1.5/3] Compiling lwext4 library natively...
    cl /nologo /W0 /O2 /MD /c /D_CRT_SECURE_NO_WARNINGS ^
        /I"%LWEXT4%\include" ^
        /I"%LWEXT4%\include\generated" ^
        "%LWEXT4%\src\*.c" /Fo"%BUILD%\lwext4\\"
    lib /nologo /out:"%BUILD%\lwext4.lib" "%BUILD%\lwext4\*.obj"
)

:: ─── Source list ─────────────────────────────────────────────────────────
set SOURCES=^
    "%SRC%\main.cpp" ^
    "%SRC%\RootTool.cpp" ^
    "%SRC%\VHDManager.cpp" ^
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
    /I"%LWEXT4%\include" ^
    /I"%LWEXT4%\include\generated"

:: ─── Libraries ──────────────────────────────────────────────────────────
set LIBS=d3d11.lib dxgi.lib advapi32.lib shell32.lib dwmapi.lib "%BUILD%\lwext4.lib"

:: ─── Compile & Link ─────────────────────────────────────────────────────
echo [2/3] Compiling...
cl /nologo /W3 /O2 /Zi /MD /EHsc /std:c++17 ^
    %INCLUDES% %SOURCES% "%BUILD%\resources.res" ^
    /Fo"%BUILD%\\" ^
    /Fe"%BUILD%\BstkRooter.exe" ^
    /Fd"%BUILD%\BstkRooter.pdb" ^
    /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /DEBUG /OPT:REF /OPT:ICF %LIBS%

if %errorlevel% neq 0 (
    echo [Build] FAILED.
    exit /b 1
)

echo [3/3] Done.
echo.
echo   Output: %BUILD%\BstkRooter.exe
