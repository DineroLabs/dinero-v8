@echo off
REM 🚀 Dinero Cryptocurrency - Windows Build Script
REM This script builds Dinero on Windows

echo 🚀 Building Dinero Cryptocurrency on Windows...
echo ==============================================

REM Check prerequisites
echo 🔍 Checking prerequisites...

REM Check if Visual Studio is available
where cl >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo ❌ Visual Studio C++ compiler not found!
    echo    Install Visual Studio 2019/2022 with C++ workload
    echo    Or install Build Tools for Visual Studio
    pause
    exit /b 1
)

REM Check if CMake is installed
where cmake >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo ❌ CMake not found!
    echo    Install from: https://cmake.org/download/
    echo    Or install via: winget install Kitware.CMake
    pause
    exit /b 1
)

REM Check if Qt6 is available (optional)
set QT_PATH=
if exist "C:\Qt\6.9.1\msvc2019_64" (
    set QT_PATH=C:\Qt\6.9.1\msvc2019_64
    echo ✅ Found Qt6 at: %QT_PATH%
) else if exist "C:\Qt\6.9.1\msvc2022_64" (
    set QT_PATH=C:\Qt\6.9.1\msvc2022_64
    echo ✅ Found Qt6 at: %QT_PATH%
) else (
    echo ⚠️  Qt6 not found - GUI components will not be built
    echo    Install Qt6 for GUI support from: https://www.qt.io/download
)

echo ✅ Prerequisites check passed!

REM Create build directory
echo 📁 Creating build directory...
if exist build rmdir /s /q build
mkdir build
cd build

REM Configure build
echo ⚙️  Configuring build...
if defined QT_PATH (
    cmake .. -G "Visual Studio 16 2019" -A x64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_PATH%" -DENABLE_SANITIZERS=OFF
) else (
    cmake .. -G "Visual Studio 16 2019" -A x64 -DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=OFF -DBUILD_GUI=OFF
)

REM Build
echo 🔨 Building Dinero...
cmake --build . --config Release

echo ✅ Build completed successfully!
echo.
echo 🚀 Your Dinero binaries are ready in: .\build\bin\Release\
echo.
echo 📋 Next steps:
echo 1. Create data directory: mkdir data
echo 2. Start daemon: .\build\bin\Release\dinerod.exe -datadir=.\data -rpcport=20998 -port=20999 -daemon=0 -server=1
echo 3. Create wallet and start mining!
echo.
echo 📚 See README.md for complete usage instructions
echo ⚡ See QUICK_START.md for essential commands

pause
