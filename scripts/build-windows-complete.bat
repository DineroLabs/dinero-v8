@echo off
REM 🚀 Complete Windows Build & Package Script for Dinero
REM Creates a ready-to-distribute Windows application with installer

echo 🚀 Dinero Complete Windows Build & Package
echo ==========================================

REM Set script directory
set SCRIPT_DIR=%~dp0
set PROJECT_ROOT=%SCRIPT_DIR%..
cd /d "%PROJECT_ROOT%"

echo 📁 Project root: %CD%

REM Check prerequisites
echo 🔍 Checking prerequisites...

REM Check Visual Studio
where cl >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo ❌ Visual Studio C++ compiler not found!
    echo    Run from Visual Studio Developer Command Prompt
    pause
    exit /b 1
)

REM Check CMake
where cmake >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo ❌ CMake not found!
    pause
    exit /b 1
)

REM Check Qt6
set QT_PATH=
if exist "C:\Qt\6.9.1\msvc2019_64" (
    set QT_PATH=C:\Qt\6.9.1\msvc2019_64
) else if exist "C:\Qt\6.9.1\msvc2022_64" (
    set QT_PATH=C:\Qt\6.9.1\msvc2022_64
) else if exist "C:\Qt\6.7.2\msvc2022_64" (
    set QT_PATH=C:\Qt\6.7.2\msvc2022_64
) else (
    echo ❌ Qt6 not found! Please install Qt6 for Windows
    echo    Download from: https://www.qt.io/download
    pause
    exit /b 1
)

echo ✅ Found Qt6 at: %QT_PATH%

REM Step 1: Create Windows icon
echo 🎨 Creating Windows icon...
python scripts\create-windows-icon.py
if %ERRORLEVEL% neq 0 (
    echo ⚠️  Warning: Could not create .ico file (continuing with .png)
    echo    Install Pillow: pip install Pillow
)

REM Step 2: Clean and create build directory
echo 📁 Setting up build directory...
if exist build-windows rmdir /s /q build-windows
mkdir build-windows
cd build-windows

REM Step 3: Configure with Qt6 and Windows resources
echo ⚙️  Configuring build with Qt6...
cmake .. ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QT_PATH%" ^
    -DENABLE_SANITIZERS=OFF ^
    -DBUILD_GUI=ON ^
    -DSTATIC_BUILD=ON

if %ERRORLEVEL% neq 0 (
    echo ❌ CMake configuration failed!
    pause
    exit /b 1
)

REM Step 4: Build the application
echo 🔨 Building Dinero with Qt6 GUI...
cmake --build . --config Release --parallel

if %ERRORLEVEL% neq 0 (
    echo ❌ Build failed!
    pause
    exit /b 1
)

echo ✅ Build completed successfully!

REM Step 5: Deploy Qt dependencies
echo 📦 Deploying Qt dependencies...
"%QT_PATH%\bin\windeployqt.exe" ^
    --release ^
    --no-translations ^
    --no-system-d3d-compiler ^
    --no-opengl-sw ^
    bin\Release\dinero-qt6.exe

if %ERRORLEVEL% neq 0 (
    echo ⚠️  Warning: Qt deployment may have issues (continuing...)
)

REM Step 6: Copy additional files
echo 📋 Copying additional files...
copy "..\README.md" "bin\Release\" >nul
copy "..\LICENSE" "bin\Release\" >nul

REM Step 7: Create portable package
echo 📦 Creating portable package...
cd bin\Release
if exist "..\..\DineroCoin-Portable-Windows.zip" del "..\..\DineroCoin-Portable-Windows.zip"
powershell -Command "Compress-Archive -Path '.' -DestinationPath '..\..\DineroCoin-Portable-Windows.zip'"
cd ..\..

REM Step 8: Create installer (if NSIS is available)
echo 🏗️  Creating installer...
where makensis >nul 2>nul
if %ERRORLEVEL% equ 0 (
    echo ✅ NSIS found, creating installer...
    makensis scripts\dinero-installer.nsi
    if %ERRORLEVEL% equ 0 (
        echo ✅ Installer created successfully!
    ) else (
        echo ⚠️  Installer creation failed
    )
) else (
    echo ⚠️  NSIS not found - skipping installer creation
    echo    Install NSIS from: https://nsis.sourceforge.io/
)

REM Step 9: Summary
echo.
echo 🎉 Windows build completed successfully!
echo.
echo 📦 Deliverables:
echo    • Executable: build-windows\bin\Release\dinero-qt6.exe
echo    • Portable ZIP: build-windows\DineroCoin-Portable-Windows.zip
if exist "DineroCoin-*-Windows-Setup.exe" (
    echo    • Installer: DineroCoin-*-Windows-Setup.exe
)
echo.
echo 🚀 Ready for distribution!
echo.
echo 📋 To test:
echo    1. Run: build-windows\bin\Release\dinero-qt6.exe
echo    2. Or extract and run the portable ZIP
echo    3. Or run the installer (if created)
echo.

pause
