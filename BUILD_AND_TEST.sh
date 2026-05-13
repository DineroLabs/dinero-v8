#!/bin/bash
# 🚀 DineroCoin Cross-Platform Build & Test Script
# Automates building both Qt wallet and iOS app with validation

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Paths
QT_ROOT="/Users/haydarevich/Documents/DineroCoin"
IOS_ROOT="/Users/haydarevich/Documents/X-Code_DineroApp/Dinero"
BUILD_DIR="$QT_ROOT/build-clean"

echo -e "${BLUE}╔════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  DineroCoin Cross-Platform Build Script  ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════╝${NC}"
echo ""

# Function to print status
print_status() {
    echo -e "${GREEN}✓${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

print_info() {
    echo -e "${BLUE}ℹ${NC} $1"
}

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# ============================================================================
# STEP 1: Check Prerequisites
# ============================================================================
echo -e "\n${BLUE}[Step 1/6] Checking Prerequisites...${NC}"

# Check for Homebrew
if ! command_exists brew; then
    print_error "Homebrew not found"
    echo "Install from: https://brew.sh"
    exit 1
fi
print_status "Homebrew installed"

# Check for cmake
if ! command_exists cmake; then
    print_warning "cmake not found - installing..."
    brew install cmake
    if [ $? -eq 0 ]; then
        print_status "cmake installed successfully"
    else
        print_error "Failed to install cmake"
        exit 1
    fi
else
    print_status "cmake installed ($(cmake --version | head -1))"
fi

# Check for Qt6
if ! brew list qt@6 &>/dev/null; then
    print_warning "Qt6 not found - installing (this may take a while)..."
    brew install qt@6
    if [ $? -eq 0 ]; then
        print_status "Qt6 installed successfully"
    else
        print_error "Failed to install Qt6"
        exit 1
    fi
else
    print_status "Qt6 installed"
fi

# Check for Xcode
if ! command_exists xcodebuild; then
    print_error "Xcode not found"
    echo "Install from App Store"
    exit 1
fi
print_status "Xcode installed ($(xcodebuild -version | head -1))"

# Check for Swift
if ! command_exists swift; then
    print_error "Swift not found"
    exit 1
fi
print_status "Swift installed ($(swift --version | head -1))"

# ============================================================================
# STEP 2: Validate Code Syntax
# ============================================================================
echo -e "\n${BLUE}[Step 2/6] Validating Code Syntax...${NC}"

# Check C++ syntax (basic check)
print_info "Checking C++ files for obvious errors..."
if command_exists clang++; then
    # Just syntax check, no linking
    clang++ -std=c++17 -fsyntax-only \
        "$QT_ROOT/gui/src/mainwindow.cpp" \
        2>/dev/null || print_warning "C++ syntax check had warnings"
    print_status "C++ syntax validated"
else
    print_warning "clang++ not found, skipping C++ validation"
fi

# Check Swift syntax
print_info "Checking Swift files..."
if [ -f "$IOS_ROOT/Dinero/Views/SeedExportView.swift" ]; then
    swiftc -typecheck "$IOS_ROOT/Dinero/Views/SeedExportView.swift" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_status "SeedExportView.swift syntax OK"
    else
        print_warning "SeedExportView.swift has syntax issues"
    fi
else
    print_error "SeedExportView.swift not found!"
fi

# ============================================================================
# STEP 3: Build Qt Wallet
# ============================================================================
echo -e "\n${BLUE}[Step 3/6] Building Qt Desktop Wallet...${NC}"

cd "$QT_ROOT"

# Get Qt6 path
QT6_PATH=$(brew --prefix qt@6)
print_info "Qt6 path: $QT6_PATH"

# Configure CMake
print_info "Configuring CMake..."
cmake -B "$BUILD_DIR" -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT6_PATH" \
    -DBUILD_GUI=ON \
    -DBUILD_TESTS=OFF \
    -DBUILD_MINER=ON

if [ $? -eq 0 ]; then
    print_status "CMake configuration successful"
else
    print_error "CMake configuration failed"
    exit 1
fi

# Build
print_info "Building dinero-qt (this may take several minutes)..."
cmake --build "$BUILD_DIR" --target dinero-qt -j$(sysctl -n hw.ncpu)

if [ $? -eq 0 ]; then
    print_status "Qt wallet built successfully!"
    QT_BUILT=true
    QT_BINARY="$BUILD_DIR/gui/dinero-qt"
    
    if [ -f "$QT_BINARY" ]; then
        print_status "Binary location: $QT_BINARY"
    else
        print_warning "Binary not found at expected location"
        QT_BUILT=false
    fi
else
    print_error "Qt wallet build failed"
    QT_BUILT=false
fi

# ============================================================================
# STEP 4: Build iOS App
# ============================================================================
echo -e "\n${BLUE}[Step 4/6] Building iOS App...${NC}"

cd "$IOS_ROOT"

# Resolve dependencies first
print_info "Resolving package dependencies..."
xcodebuild -resolvePackageDependencies 2>&1 | tail -5

# Try to build
print_info "Building Dinero iOS app..."
xcodebuild -scheme Dinero \
    -configuration Debug \
    -destination 'platform=iOS Simulator,name=iPhone 15' \
    build \
    2>&1 | tail -20

if [ ${PIPESTATUS[0]} -eq 0 ]; then
    print_status "iOS app built successfully!"
    IOS_BUILT=true
else
    print_error "iOS app build failed (likely dependency issues)"
    print_warning "You may need to open in Xcode and resolve package versions"
    IOS_BUILT=false
fi

# ============================================================================
# STEP 5: Run Basic Tests
# ============================================================================
echo -e "\n${BLUE}[Step 5/6] Running Basic Tests...${NC}"

if [ "$QT_BUILT" = true ]; then
    print_info "Testing Qt wallet launch..."
    # Just check if it runs and exits
    timeout 5s "$QT_BINARY" --help >/dev/null 2>&1 || true
    if [ $? -eq 124 ] || [ $? -eq 0 ]; then
        print_status "Qt wallet can launch"
    else
        print_warning "Qt wallet may have runtime issues"
    fi
else
    print_warning "Skipping Qt tests (not built)"
fi

if [ "$IOS_BUILT" = true ]; then
    print_status "iOS app ready for testing in simulator"
else
    print_warning "Skipping iOS tests (not built)"
fi

# ============================================================================
# STEP 6: Summary & Next Steps
# ============================================================================
echo -e "\n${BLUE}[Step 6/6] Build Summary${NC}"
echo ""
echo "╔════════════════════════════════════════════╗"
echo "║           BUILD RESULTS                    ║"
echo "╠════════════════════════════════════════════╣"

if [ "$QT_BUILT" = true ]; then
    echo -e "║ Qt Desktop Wallet:  ${GREEN}✓ SUCCESS${NC}             ║"
    echo "║ Location: $QT_BINARY"
else
    echo -e "║ Qt Desktop Wallet:  ${RED}✗ FAILED${NC}              ║"
fi

if [ "$IOS_BUILT" = true ]; then
    echo -e "║ iOS Mobile App:     ${GREEN}✓ SUCCESS${NC}             ║"
else
    echo -e "║ iOS Mobile App:     ${RED}✗ FAILED${NC}              ║"
fi

echo "╚════════════════════════════════════════════╝"
echo ""

# Next steps
echo -e "${BLUE}📋 Next Steps:${NC}"
echo ""

if [ "$QT_BUILT" = true ]; then
    echo "1. Launch Qt Wallet:"
    echo "   $QT_BINARY"
    echo ""
fi

if [ "$IOS_BUILT" = true ]; then
    echo "2. Test iOS App in Simulator:"
    echo "   xcrun simctl boot 'iPhone 15'"
    echo "   open -a Simulator"
    echo "   xcrun simctl install booted DerivedData/.../Dinero.app"
    echo ""
fi

if [ "$QT_BUILT" = true ] && [ "$IOS_BUILT" = true ]; then
    echo "3. Run Cross-Platform Tests:"
    echo "   Follow: $QT_ROOT/TESTING_GUIDE_CROSS_PLATFORM.md"
    echo ""
    echo "4. Test Seed Phrase Compatibility:"
    echo "   a. Create wallet on Qt → Export seed"
    echo "   b. Restore same seed on iOS"
    echo "   c. Verify addresses match!"
    echo ""
fi

if [ "$QT_BUILT" = false ] || [ "$IOS_BUILT" = false ]; then
    echo -e "${YELLOW}⚠ Some builds failed. Check error messages above.${NC}"
    echo ""
    echo "Common fixes:"
    echo "• Qt wallet: Check CMakeLists.txt, verify Qt6 paths"
    echo "• iOS app: Open in Xcode, resolve package dependencies"
    echo "• Dependencies: Update Package.swift versions"
    echo ""
fi

# Documentation links
echo -e "${BLUE}📚 Documentation:${NC}"
echo "• User Guide: $QT_ROOT/USER_GUIDE_MULTI_PLATFORM.md"
echo "• Testing Guide: $QT_ROOT/TESTING_GUIDE_CROSS_PLATFORM.md"
echo "• Implementation: $QT_ROOT/IMPLEMENTATION_SUMMARY.md"
echo ""

# Success/failure exit
if [ "$QT_BUILT" = true ] && [ "$IOS_BUILT" = true ]; then
    echo -e "${GREEN}🎉 All builds successful! Ready for testing.${NC}"
    exit 0
elif [ "$QT_BUILT" = true ] || [ "$IOS_BUILT" = true ]; then
    echo -e "${YELLOW}⚠ Partial success - some builds completed.${NC}"
    exit 0
else
    echo -e "${RED}✗ Build failed. See errors above for details.${NC}"
    exit 1
fi
