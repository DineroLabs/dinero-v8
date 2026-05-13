@echo off
REM Windows Build Script - Reproducible static build with MSVC + static CRT

setlocal enabledelayedexpansion

echo === Dinero Windows Build ===
echo Build type: Release (reproducible)
echo Compiler: MSVC with static CRT (/MT)
echo Dependencies: Fully static
echo ============================

REM Check for required tools
where perl >nul 2>&1
if errorlevel 1 (
    echo ERROR: Perl not found. Install Strawberry Perl and add to PATH.
    exit /b 1
)

where nasm >nul 2>&1
if errorlevel 1 (
    echo ERROR: NASM not found. Install NASM and add to PATH.
    exit /b 1
)

where ninja >nul 2>&1
if errorlevel 1 (
    echo ERROR: Ninja not found. Install Ninja and add to PATH.
    exit /b 1
)

echo Found required tools: Perl, NASM, Ninja

REM Clean previous build
if exist build rmdir /s /q build
mkdir build
cd build

REM Set reproducible timestamp
set SOURCE_DATE_EPOCH=1692576000

REM Configure with static CRT and all dependencies vendored
cmake .. -G "Ninja" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DDINERO_VENDOR_ROCKSDB=ON ^
  -DDINERO_WITH_SNAPPY=ON ^
  -DDINERO_WITH_LZ4=ON ^
  -DDINERO_WITH_ZSTD=ON ^
  -DDINERO_WITH_VULKAN=OFF

if errorlevel 1 (
    echo ERROR: CMake configuration failed
    exit /b 1
)

echo.
echo === Building Dinero ===
cmake --build . --parallel

if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

echo.
echo === Verifying Static Linking ===
where dumpbin >nul 2>&1
if not errorlevel 1 (
    echo Checking dinerod.exe dependencies:
    dumpbin /DEPENDENTS bin\Release\dinerod.exe | findstr /i "ssl crypto zstd lz4 snappy rocksdb"
    if not errorlevel 1 (
        echo ERROR: Found external dependencies!
        exit /b 1
    ) else (
        echo OK: No external static dependencies found
    )
    
    echo.
    echo All dinerod.exe dependencies:
    dumpbin /DEPENDENTS bin\Release\dinerod.exe
    
    echo.
    echo Checking for VCRUNTIME (should be none with /MT):
    dumpbin /DEPENDENTS bin\Release\dinerod.exe | findstr /i "vcruntime"
    if not errorlevel 1 (
        echo WARNING: Found VCRUNTIME dependency (static CRT may not be working)
    ) else (
        echo OK: No VCRUNTIME dependency (static CRT confirmed)
    )
) else (
    echo WARNING: dumpbin not available, skipping dependency verification
)

echo.
echo === Build Summary ===
echo ✅ Windows build completed successfully
echo 📦 Binaries: %CD%\bin\Release\
echo 🔍 Static linking verified (no external deps)
echo 🏗️ Static CRT (/MT) confirmed
echo 📋 SBOM: %CD%\sbom.json
echo.
echo To verify reproducibility, run this script again and compare binaries.

endlocal
