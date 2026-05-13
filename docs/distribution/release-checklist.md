# Dinero Release Checklist
_Version: TEMPLATE • Last updated: August 20, 2025_

## 0) Preconditions (freeze window)
- [ ] Changelog updated; version bump committed (`VERSION`/`CMakeLists.txt`/Info.plist as needed)
- [ ] `SOURCE_DATE_EPOCH` chosen and frozen for this release
- [ ] Toolchain digests recorded (compilers, container images, SDKs)
- [ ] Deps pinned: OpenSSL, RocksDB, Snappy, LZ4, Zstd, JsonCpp (exact commit/tag + SHA256 for tarballs)
- [ ] Fuzzers green (last 24–48h), sanitizers green (ASAN/UBSAN/TSAN)
- [ ] Performance gates green (IBD throughput, block validation latency, compaction metrics)
- [ ] Security advisories reviewed; CVE backlog triaged

## 1) Build matrix (reproducible)

### macOS (universal)
```bash
rm -rf build && mkdir build
SOURCE_DATE_EPOCH=<epoch> cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_AR="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-ar" \
  -DCMAKE_RANLIB="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-ranlib" \
  -DCMAKE_PREFIX_PATH="/Applications/Qt/6.7.2/macos" \
  -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew" \
  -DDINERO_VENDOR_ROCKSDB=ON -DDINERO_WITH_SNAPPY=ON -DDINERO_WITH_LZ4=ON -DDINERO_WITH_ZSTD=ON
cmake --build build --parallel
```

### Linux (glibc ≥ 2.17 target; static deps, dynamic glibc)
```bash
docker run --rm -v $PWD:/src -w /src quay.io/pypa/manylinux2014_x86_64 /bin/bash -lc '
  rm -rf build && mkdir build && \
  SOURCE_DATE_EPOCH=<epoch> cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="/opt/Qt/6.7.2/gcc_64" \
    -DDINERO_VENDOR_ROCKSDB=ON -DDINERO_WITH_SNAPPY=ON -DDINERO_WITH_LZ4=ON -DDINERO_WITH_ZSTD=ON && \
  cmake --build build --parallel
'
```

### Windows (MSVC + static CRT)
```cmd
rmdir /s /q build & mkdir build
set SOURCE_DATE_EPOCH=<epoch>
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="C:\Qt\6.7.2\msvc2022_64" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DDINERO_VENDOR_ROCKSDB=ON -DDINERO_WITH_SNAPPY=ON -DDINERO_WITH_LZ4=ON -DDINERO_WITH_ZSTD=ON
cmake --build build --parallel
```

## 2) Bundle GUI (macOS) and audit
```bash
# Automatic via CMake post-build, but manual verification:
/Applications/Qt/6.7.2/macos/bin/macdeployqt build/bin/dinero-qt6.app -always-overwrite -verbose=2
scripts/macos/audit-app-clean.sh build/bin/dinero-qt6.app
```

## 3) Static core verification
```bash
# macOS
otool -L build/bin/dinerod | grep -E 'ssl|crypto|rocksdb|snappy|lz4|zstd' || echo "✅ Static"
otool -L build/bin/dinero-cli | grep -E 'ssl|crypto|rocksdb|snappy|lz4|zstd' || echo "✅ Static"

# Linux  
ldd build/bin/dinerod | grep -E 'ssl|crypto|rocksdb|snappy|lz4|zstd' || echo "✅ Static"
ldd build/bin/dinero-cli | grep -E 'ssl|crypto|rocksdb|snappy|lz4|zstd' || echo "✅ Static"

# Windows
dumpbin /DEPENDENTS build\bin\dinerod.exe | findstr /i "ssl crypto rocksdb" || echo "✅ Static"
dumpbin /DEPENDENTS build\bin\dinero-cli.exe | findstr /i "ssl crypto rocksdb" || echo "✅ Static"
```

## 4) Universal binary verification (macOS)
```bash
lipo -info build/deps/openssl/lib/libcrypto.a
lipo -info build/deps/openssl/lib/libssl.a
lipo -info build/_deps/rocksdb-v9.1.0-install/universal/lib/librocksdb.a
# Should all show: Architectures in the fat file: arm64 x86_64
```

## 5) GUI hermetic verification
```bash
# macOS - should show @executable_path/../Frameworks/Qt*.framework paths
otool -L build/bin/dinero-qt6.app/Contents/MacOS/dinero-qt6 | grep Qt

# Windows - should show no Qt DLLs (bundled via windeployqt)
dumpbin /DEPENDENTS build\bin\dinero-qt6.exe | findstr /i "qt"

# Linux - should show no system Qt libraries  
ldd build/bin/dinero-qt6 | grep -i qt || echo "✅ No system Qt"
```

## 6) SBOM and checksums
```bash
# SBOM automatically generated during Release builds
ls -la build/sbom.json

# Generate checksums
cd build/bin
sha256sum * > SHA256SUMS.txt
# macOS: shasum -a 256 * > SHA256SUMS.txt
```

## 7) Signing (platform-specific)
### macOS
```bash
# Code sign binaries and app bundles
codesign --sign "Developer ID Application: Your Name" build/bin/dinerod
codesign --sign "Developer ID Application: Your Name" build/bin/dinero-cli  
codesign --sign "Developer ID Application: Your Name" build/bin/dinero-qt6.app

# Create and sign DMG
hdiutil create -volname "Dinero" -srcfolder build/bin -ov -format UDZO dinero-macos.dmg
codesign --sign "Developer ID Application: Your Name" dinero-macos.dmg

# Notarize (requires Apple Developer account)
xcrun notarytool submit dinero-macos.dmg --keychain-profile "notarytool-profile" --wait
xcrun stapler staple dinero-macos.dmg
```

### Windows
```cmd
# Sign executables (requires code signing certificate)
signtool sign /f certificate.p12 /p password /t http://timestamp.digicert.com build\bin\dinerod.exe
signtool sign /f certificate.p12 /p password /t http://timestamp.digicert.com build\bin\dinero-cli.exe
signtool sign /f certificate.p12 /p password /t http://timestamp.digicert.com build\bin\dinero-qt6.exe
```

### Linux
```bash
# Create AppImage (optional)
# Sign with GPG (standard for Linux distributions)
gpg --armor --detach-sign build/bin/dinero-qt6
```

## 8) PGP signatures
```bash
# Sign checksums file
gpg --armor --detach-sign SHA256SUMS.txt

# Sign release archives
gpg --armor --detach-sign dinero-macos.dmg
gpg --armor --detach-sign dinero-windows.zip  
gpg --armor --detach-sign dinero-linux.tar.gz
```

## 9) Reproducibility verification
```bash
# Fresh clone build (different machine/container)
git clone --depth 1 --branch v<version> https://github.com/dinero/dinero.git fresh-build
cd fresh-build
SOURCE_DATE_EPOCH=<same-epoch> <same-cmake-command>
cmake --build build --parallel

# Compare binaries (should be identical)
diff -r build/bin ../original-build/bin || echo "❌ Not reproducible"
```

## 10) Release artifacts checklist
- [ ] `dinero-<version>-macos-universal.dmg` (signed + notarized)
- [ ] `dinero-<version>-windows-x64.zip` (signed executables)
- [ ] `dinero-<version>-linux-x64.tar.gz` 
- [ ] `SHA256SUMS.txt` (checksums for all artifacts)
- [ ] `SHA256SUMS.txt.asc` (PGP signature of checksums)
- [ ] `sbom.json` (Software Bill of Materials)
- [ ] Individual `.asc` files for each major artifact
- [ ] Release notes with upgrade instructions

## 11) Final verification
- [ ] Download artifacts from release page
- [ ] Verify PGP signatures: `gpg --verify SHA256SUMS.txt.asc`
- [ ] Verify checksums: `sha256sum -c SHA256SUMS.txt`
- [ ] Test fresh OS installs (VM/container)
- [ ] Verify SBOM completeness and accuracy
- [ ] Check all links in release notes

## 12) Post-release
- [ ] Update website download links
- [ ] Notify package maintainers (Homebrew, AUR, etc.)
- [ ] Update documentation with new version
- [ ] Archive build environment details
- [ ] Security advisory notifications (if applicable)

---

**Note**: Replace `<epoch>` with actual `SOURCE_DATE_EPOCH` value, `<version>` with release version, and update Qt paths as needed.
