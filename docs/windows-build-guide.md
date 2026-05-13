# Dinero Windows Build Guide

This guide covers building Dinero on Windows with static dependencies for maximum portability.

## Prerequisites

### Required Tools

1. **Visual Studio 2019 or 2022** with C++ development tools
   - Install "Desktop development with C++" workload
   - Includes MSVC compiler and Windows SDK

2. **Strawberry Perl** (required for OpenSSL build)
   ```powershell
   # Download from: https://strawberryperl.com/
   # Or install via Chocolatey:
   choco install strawberryperl
   ```

3. **NASM** (Netwide Assembler, required for OpenSSL)
   ```powershell
   # Download from: https://www.nasm.us/
   # Or install via Chocolatey:
   choco install nasm
   ```

4. **CMake 3.20+**
   ```powershell
   # Download from: https://cmake.org/download/
   # Or install via Chocolatey:
   choco install cmake
   ```

5. **Git** (for cloning repository)
   ```powershell
   # Download from: https://git-scm.com/
   # Or install via Chocolatey:
   choco install git
   ```

### Environment Setup

1. **Open Visual Studio Developer Command Prompt**
   - Start Menu → Visual Studio 2022 → Developer Command Prompt
   - This ensures `nmake` and MSVC are in PATH

2. **Verify Prerequisites**
   ```cmd
   perl --version
   nasm -v
   cmake --version
   nmake /?
   ```

## Build Process

### 1. Clone Repository
```cmd
git clone https://github.com/your-org/DineroCoin.git
cd DineroCoin
```

### 2. Configure Build
```cmd
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -G "NMake Makefiles"
```

**Alternative: Visual Studio Solution**
```cmd
cmake .. -DCMAKE_BUILD_TYPE=Release -G "Visual Studio 17 2022" -A x64
```

### 3. Build
```cmd
# For NMake:
nmake

# For Visual Studio:
cmake --build . --config Release
```

## Build Features

### Static Dependencies
- **OpenSSL 3.3.1**: Vendored and statically linked with `/MT` CRT
- **RocksDB 9.1.0**: Vendored and statically linked with `/MT` CRT
- **JsonCpp**: Built from source (static)
- **System Libraries**: ws2_32, crypt32, bcrypt, rpcrt4, shlwapi

### Compiler Settings
- **Runtime**: Static CRT (`/MT` for Release, `/MTd` for Debug)
- **Target**: Windows 7+ API (`_WIN32_WINNT=0x0601`)
- **Definitions**: `WIN32_LEAN_AND_MEAN`, `NOMINMAX`

### Build Outputs
```
build/
├── bin/
│   ├── dinerod.exe      # Main daemon
│   └── dinero-cli.exe   # Command-line client
└── lib/
    └── *.lib            # Static libraries
```

## Verification

### 1. Check Dependencies
```cmd
# Should show only system DLLs, no OpenSSL/RocksDB dependencies
dumpbin /dependents bin\dinerod.exe
```

### 2. Test Basic Functionality
```cmd
# Version check
bin\dinerod.exe -version

# Quick regtest
bin\dinerod.exe -regtest -server=1 -datadir=C:\temp\dinero-regtest -daemon
bin\dinero-cli.exe -regtest -datadir=C:\temp\dinero-regtest getblockcount
bin\dinero-cli.exe -regtest -datadir=C:\temp\dinero-regtest stop
```

## Troubleshooting

### Common Issues

**1. "perl: command not found"**
- Install Strawberry Perl and ensure it's in PATH
- Restart Developer Command Prompt after installation

**2. "nasm: command not found"**
- Install NASM and add to PATH: `C:\Program Files\NASM`
- Restart Developer Command Prompt

**3. "nmake: command not found"**
- Use Visual Studio Developer Command Prompt (not regular cmd)
- Ensure Visual Studio C++ tools are installed

**4. OpenSSL configure fails**
- Verify Perl and NASM are in PATH
- Check that you're using Developer Command Prompt
- Try cleaning build directory: `rmdir /s build && mkdir build`

**5. Linking errors with OpenSSL**
- Ensure static CRT is used (`/MT`)
- Check that Windows system libraries are linked (ws2_32, crypt32, etc.)

### Debug Build
```cmd
cmake .. -DCMAKE_BUILD_TYPE=Debug -G "NMake Makefiles"
nmake
```

### Clean Build
```cmd
cd ..
rmdir /s build
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -G "NMake Makefiles"
nmake
```

## Advanced Configuration

### Custom OpenSSL Version
```cmd
cmake .. -DOPENSSL_VERSION=3.3.2 -DCMAKE_BUILD_TYPE=Release
```

### Enable Additional Features
```cmd
cmake .. -DBUILD_RPCD=ON -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
```

### Cross-Compilation (x86 on x64)
```cmd
# Use x86 Developer Command Prompt
cmake .. -DCMAKE_BUILD_TYPE=Release -G "NMake Makefiles"
```

## CI/CD Integration

### GitHub Actions Example
```yaml
name: Windows Build
on: [push, pull_request]

jobs:
  windows:
    runs-on: windows-2022
    steps:
    - uses: actions/checkout@v4
    
    - name: Setup Perl
      uses: shogo82148/actions-setup-perl@v1
      with:
        perl-version: '5.32'
    
    - name: Setup NASM
      uses: ilammy/setup-nasm@v1
    
    - name: Setup MSVC
      uses: ilammy/msvc-dev-cmd@v1
    
    - name: Configure
      run: |
        mkdir build
        cd build
        cmake .. -DCMAKE_BUILD_TYPE=Release -G "NMake Makefiles"
    
    - name: Build
      run: |
        cd build
        nmake
    
    - name: Test
      run: |
        cd build
        bin\dinerod.exe -version
```

## Distribution

### Creating Release Package
```cmd
# After successful build
mkdir dinero-windows-x64
copy bin\dinerod.exe dinero-windows-x64\
copy bin\dinero-cli.exe dinero-windows-x64\
copy ..\README.md dinero-windows-x64\
copy ..\LICENSE dinero-windows-x64\

# Create ZIP
powershell Compress-Archive -Path dinero-windows-x64 -DestinationPath dinero-windows-x64.zip
```

### Checksums
```cmd
# Generate SHA256 checksums
certutil -hashfile dinero-windows-x64.zip SHA256 > dinero-windows-x64.zip.sha256
```

## Performance Notes

- Static linking increases binary size but eliminates runtime dependencies
- OpenSSL 3.x provides better performance than 1.1.x on modern CPUs
- Release builds are significantly faster than Debug builds
- Consider using `/O2` optimization for production builds

## Security Considerations

- Static CRT (`/MT`) prevents DLL hijacking attacks
- No external DLL dependencies reduce attack surface
- OpenSSL 3.3.1 includes latest security patches
- Windows Defender may flag cryptocurrency software - add exclusions if needed

---

*Last updated: August 20, 2025*
*Tested with: Visual Studio 2022, Windows 11, OpenSSL 3.3.1*
