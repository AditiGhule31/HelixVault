@echo off
REM =============================================================================
REM  build_msvc.bat — Build HelixVault with MSVC (cl.exe)
REM
REM  Run this script from a "Developer Command Prompt for VS 20xx" so that
REM  cl.exe, link.exe, and the Windows SDK headers are on the PATH.
REM
REM  Usage:
REM    build_msvc.bat          — Release build
REM    build_msvc.bat debug    — Debug build (no optimisation)
REM    build_msvc.bat clean    — Remove build artefacts
REM =============================================================================

setlocal EnableDelayedExpansion

set BINARY=helixvault.exe
set SRC_DIR=src
set INC_DIR=include
set BUILD_DIR=build_msvc
set OBJ_DIR=%BUILD_DIR%\obj

REM ── Handle 'clean' ────────────────────────────────────────────────────────
if "%1"=="clean" (
    echo [clean] Removing %BUILD_DIR% and %BINARY% ...
    if exist %BUILD_DIR% rd /s /q %BUILD_DIR%
    if exist %BINARY%    del /q %BINARY%
    if exist storage_root rd /s /q storage_root
    echo Done.
    exit /b 0
)

REM ── Create output directories ─────────────────────────────────────────────
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
if not exist %OBJ_DIR%   mkdir %OBJ_DIR%

REM ── Compiler flags ────────────────────────────────────────────────────────
REM  /W3        — Warning level 3
REM  /WX-       — Don't treat warnings as errors (relaxed for portability)
REM  /wd4996    — Suppress POSIX name deprecation (fopen, strdup, etc.)
REM  /wd4200    — Suppress zero-sized array in struct warning
REM  /wd4214    — Suppress bit field in non-int type warning
REM  /I         — Include path
REM  /D_CRT_SECURE_NO_WARNINGS — Suppress CRT security warnings
REM  /D_CRT_NONSTDC_NO_WARNINGS — Suppress POSIX naming warnings
REM  /DWIN32    — Ensure _WIN32 is defined (usually automatic, but explicit)

set COMMON_FLAGS=/nologo /W3 /WX- /wd4996 /wd4200 /wd4214 ^
                 /I%INC_DIR% ^
                 /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS /DWIN32

if "%1"=="debug" (
    echo [build] Debug build ...
    set CFLAGS=%COMMON_FLAGS% /Od /Zi /RTC1 /MDd
    set LDFLAGS=/debug
) else (
    echo [build] Release build ...
    set CFLAGS=%COMMON_FLAGS% /O2 /DNDEBUG /MD
    set LDFLAGS=
)

REM ── Compile source files ──────────────────────────────────────────────────
set OBJECTS=

for %%f in (%SRC_DIR%\*.c) do (
    set SRC=%%f
    set OBJ=%OBJ_DIR%\%%~nf.obj
    echo [CC]  %%f
    cl %CFLAGS% /c %%f /Fo!OBJ!
    if errorlevel 1 (
        echo [ERROR] Compilation failed for %%f
        exit /b 1
    )
    set OBJECTS=!OBJECTS! !OBJ!
)

REM ── Link ─────────────────────────────────────────────────────────────────
echo [LD]  %BINARY%
link /nologo %LDFLAGS% /out:%BINARY% %OBJECTS% kernel32.lib
if errorlevel 1 (
    echo [ERROR] Link failed.
    exit /b 1
)

echo.
echo Build complete: %BINARY%
echo.
echo Quick start:
echo   %BINARY% selftest
echo   %BINARY% mb my-bucket
echo   %BINARY% put my-bucket readme.txt README.md
echo   %BINARY% ls my-bucket
echo   %BINARY% get my-bucket readme.txt out.txt
echo   %BINARY% del my-bucket readme.txt
echo.

endlocal
