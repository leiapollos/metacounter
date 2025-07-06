@echo off

REM This script must be run from a Developer Command Prompt for VS.

if not exist bin mkdir bin

echo Building metacounter for Windows...
cl /O2 /W4 /Fe:bin\metacounter.exe metacounter.c

if errorlevel 1 (
    echo Build failed.
    pause
) else (
    echo Build successful! Executable is 'bin\metacounter.exe'.
    echo Run it with: bin\metacounter.exe
)