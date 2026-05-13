@echo off
REM 🚀 Dinero Cryptocurrency - Windows Setup (Simple Version)
REM Double-click to install Dinero on Windows

echo 🚀 Dinero Cryptocurrency - Windows Setup
echo =========================================
echo.

REM Check for PowerShell
powershell -Command "Write-Host 'PowerShell available'" >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo ❌ PowerShell not found! Please install PowerShell.
    pause
    exit /b 1
)

echo ✅ Starting PowerShell installer...
echo.

REM Run the PowerShell installer
powershell -ExecutionPolicy Bypass -File "%~dp0DineroCoin-Windows-Setup.ps1"

echo.
echo 🎉 Setup completed!
pause
