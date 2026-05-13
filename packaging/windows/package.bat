@echo off
REM Windows Packaging Script for Dinero Desktop
REM Creates a signed installer ready for distribution

setlocal enabledelayedexpansion

echo 🪟 Windows Packaging for Dinero Desktop
echo ======================================
echo.

set "PROJECT_ROOT=%~dp0..\.."
set "BUILD_DIR=%PROJECT_ROOT%\build"
set "PACKAGE_DIR=%PROJECT_ROOT%\packaging\windows"
set "OUTPUT_DIR=%PACKAGE_DIR%\output"

REM Configuration
set "APP_NAME=Dinero Desktop"
set "APP_EXE=dinero-desktop.exe"
set "INSTALLER_NAME=DineroDesktop-2.1.2-Setup"

REM Clean output directory
if exist "%OUTPUT_DIR%" rmdir /s /q "%OUTPUT_DIR%"
mkdir "%OUTPUT_DIR%"

REM Check if executable exists
if not exist "%BUILD_DIR%\src\gui-desktop\%APP_EXE%" (
    echo ❌ Executable not found: %BUILD_DIR%\src\gui-desktop\%APP_EXE%
    echo Please build the application first
    exit /b 1
)

echo 📦 Step 1: Preparing application files...
xcopy "%BUILD_DIR%\src\gui-desktop\%APP_EXE%" "%OUTPUT_DIR%\" /Y

REM Deploy Qt dependencies
echo 🔧 Step 2: Deploying Qt dependencies...
if exist "%Qt6_DIR%\bin\windeployqt.exe" (
    "%Qt6_DIR%\bin\windeployqt.exe" "%OUTPUT_DIR%\%APP_EXE%" --qmldir "%PROJECT_ROOT%\src\gui-desktop"
) else (
    echo ⚠️ windeployqt.exe not found. Qt dependencies may not be bundled.
)

REM Code signing (if certificate available)
echo ✍️ Step 3: Code signing...
if exist "%PACKAGE_DIR%\certificate.p12" (
    echo 📝 Signing executable...
    signtool sign /f "%PACKAGE_DIR%\certificate.p12" /p "%CERT_PASSWORD%" /t http://timestamp.digicert.com "%OUTPUT_DIR%\%APP_EXE%"
    echo ✅ Code signing completed
) else (
    echo ⚠️ No certificate found. Executable will not be signed.
    echo For distribution, you'll need to sign with an EV certificate.
)

REM Create installer with NSIS
echo 📦 Step 4: Creating installer...
if exist "%ProgramFiles(x86)%\NSIS\makensis.exe" (
    "%ProgramFiles(x86)%\NSIS\makensis.exe" "%PACKAGE_DIR%\installer.nsi"
    echo ✅ Installer created
) else (
    echo ⚠️ NSIS not found. Install NSIS to create installer.
    echo Manual packaging: Copy contents of %OUTPUT_DIR% to distribution folder.
)

echo.
echo ✅ Windows packaging complete!
echo 📁 Output: %OUTPUT_DIR%\%INSTALLER_NAME%.exe
echo.
echo 🚀 Ready for distribution!
