@echo off
SETLOCAL

cd ..\msvc-full-features
rem DDA NetPlay: report the official upstream build this fork is based on.
rem getVersionString() appends "_netplay" at runtime, so this stays clean even
rem when the fork has local commits or a dirty tree.
set VERSION=cdda-experimental-2026-10-22-0345
findstr /c:%VERSION% ..\src\version.h > NUL 2> NUL
if %ERRORLEVEL% NEQ 0 (
echo Generating "version.h"...
echo VERSION defined as "%VERSION%"
>..\src\version.h echo #define VERSION "%VERSION%"
)
