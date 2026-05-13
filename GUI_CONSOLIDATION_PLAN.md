# GUI Projects Consolidation

**Date**: October 4, 2025  
**Goal**: Move duplicate GUI projects to duplicates folder, keep only active one

---

## 🔍 Analysis

### **GUI Projects Found** (8 total)

| Directory | Files | Last Modified | Status | Action |
|-----------|-------|---------------|--------|--------|
| `gui/` | 18 | Oct 3, 2025 | ✅ **ACTIVE** | **KEEP** |
| `dinero-qt/` | 21 | Sep 28, 2025 | 📦 Older variant | Move to duplicates |
| `grandma-qt/` | 15 | Sep 29, 2025 | 📦 Simplified GUI | Move to duplicates |
| `x-gui/` | 239 | Oct 1, 2025 | 📦 Experimental | Move to duplicates |
| `gui-test/` | 0 | Oct 2, 2025 | 🧪 Test data | Move to duplicates |
| `test-enhanced-gui/` | 0 | Sep 16, 2025 | 🧪 Test data | Move to duplicates |
| `test-gui-enhanced/` | 0 | Sep 16, 2025 | 🧪 Test data | Move to duplicates |
| `test-qt-free/` | 0 | Sep 9, 2025 | 🧪 Test data | Move to duplicates |

### **Additional GUI-related**
- `src/cli_gui/` - CLI GUI variant
- `src/grandma_gui/` - Grandma GUI sources
- `dinero-modern-gui.app` - Built app bundle

---

## ✅ **Active GUI: `gui/`**

**Why this is the main one**:
- Most recently modified (Oct 3, 2025)
- Has complete wallet integration
- Has network info RPCs
- Working with BIP39 HD wallet
- Has send transaction UI
- This is what we've been working on in recent sessions

**Structure**:
```
gui/
├── CMakeLists.txt
├── src/
│   ├── main.cpp               # Main entry point
│   ├── mainwindow.cpp         # Main window with tabs
│   ├── mainwindow.h
│   ├── rpcclient.cpp          # RPC client for dinerod
│   ├── rpcclient.h
│   ├── walletwizard.cpp       # Wallet creation/restore
│   └── walletwizard.h
├── qml/                       # QML resources
└── build/                     # Build artifacts
```

---

## 📦 **To Move to Duplicates**

### **1. dinero-qt/**
**Size**: 21 files  
**Last Modified**: Sep 28, 2025  
**Description**: Earlier Qt GUI implementation  
**Why Move**: Superseded by `gui/`

**Potentially Useful Code**:
- May have different UI patterns
- Could have alternative RPC client implementations
- Check before moving

---

### **2. grandma-qt/**
**Size**: 15 files  
**Last Modified**: Sep 29, 2025  
**Description**: Simplified "grandma-friendly" GUI  
**Why Move**: Specialized variant, not main development

**Potentially Useful Code**:
- Simplified UI patterns
- User-friendly error messages
- Big button designs
- Could inspire UX improvements

**Files**:
```
grandma-qt/
├── CMakeLists.txt
├── README.md            # Has documentation
├── Info.plist
├── src/                 # 9 files
├── scripts/             # Build scripts
└── release/             # Built binaries
```

---

### **3. x-gui/**
**Size**: 239 files (LARGEST)  
**Last Modified**: Oct 1, 2025  
**Description**: Experimental GUI with many variants  
**Why Move**: Too many files, experimental

**Subdirectories**:
- `x-gui/gui/`
- `x-gui/gui-desktop/`
- `x-gui/embedded_gui/`
- `x-gui/qt/`

**⚠️ WARNING**: 239 files - might have useful code, mine carefully!

**Potentially Useful Code**:
- Desktop integration patterns
- Embedded GUI for web
- Qt custom widgets
- Advanced features not in main GUI

---

### **4. Test Directories** (gui-test, test-*)

**gui-test/**:
- Test blockchain state
- Test wallet data
- No source code

**test-enhanced-gui/, test-gui-enhanced/, test-qt-free/**:
- Test data only
- Regtest configurations
- No source code

---

## 🗂️ **Source Code Duplicates**

### **src/cli_gui/**
- CLI-based GUI (terminal UI)
- 3 files: CliGuiMainWindow, DineroCliClient, main
- **Action**: Move to `duplicates/gui-variants/cli_gui/`

### **src/grandma_gui/**
- Simplified GUI sources
- Likely duplicate of grandma-qt/
- **Action**: Move to `duplicates/gui-variants/grandma_gui/`

---

## 🎯 **Consolidation Script**

```bash
#!/bin/bash
# Move duplicate GUI projects to duplicates folder

cd /Users/haydarevich/Documents/DineroCoin

# Create duplicates/gui-variants directory
mkdir -p duplicates/gui-variants

echo "🧹 Moving duplicate GUI projects to duplicates/"

# Move Qt GUI variants
echo "📦 Moving dinero-qt..."
mv dinero-qt duplicates/gui-variants/

echo "📦 Moving grandma-qt..."
mv grandma-qt duplicates/gui-variants/

echo "📦 Moving x-gui (239 files)..."
mv x-gui duplicates/gui-variants/

# Move test directories
echo "🧪 Moving test directories..."
mv gui-test duplicates/gui-variants/
mv test-enhanced-gui duplicates/gui-variants/
mv test-gui-enhanced duplicates/gui-variants/
mv test-qt-free duplicates/gui-variants/

# Move source code variants
echo "📂 Moving src GUI variants..."
mv src/cli_gui duplicates/gui-variants/
mv src/grandma_gui duplicates/gui-variants/

# Move built app if exists
if [ -d "dinero-modern-gui.app" ]; then
    echo "📦 Moving dinero-modern-gui.app..."
    mv dinero-modern-gui.app duplicates/gui-variants/
fi

# Move build directories
echo "🏗️  Moving build directories..."
[ -d "build-qt" ] && mv build-qt duplicates/gui-variants/
[ -d "build-gui" ] && mv build-gui duplicates/gui-variants/ || echo "  (build-gui is active, keeping)"

echo ""
echo "✅ GUI consolidation complete!"
echo ""
echo "Active GUI: gui/"
echo "Archived:   duplicates/gui-variants/"
echo ""
echo "Files moved: 7 directories + source variants"
```

---

## 📋 **Code Reuse Opportunities**

### **Before Moving, Extract Useful Patterns**

#### **From x-gui/** (239 files - richest source)

```bash
# Search for useful patterns
cd x-gui
grep -r "class.*Widget" . -l      # Custom widgets
grep -r "void.*paint" . -l        # Custom painting
grep -r "QStyle" . -l              # Styling code
grep -r "QNetworkAccessManager" . -l  # Network code
```

**Likely useful**:
- Custom Qt widgets
- Theme/styling implementations
- Advanced RPC client features
- Desktop notifications
- System tray integration

---

#### **From grandma-qt/** (Simplified patterns)

```bash
cd grandma-qt
cat src/main.cpp                  # Simplified initialization
cat README.md                     # UX philosophy
```

**Likely useful**:
- Large, accessible buttons
- Simple error messages
- Minimal UI clutter patterns
- User-friendly language

---

#### **From dinero-qt/** (Earlier implementation)

```bash
cd dinero-qt
diff -u src/main.cpp ../gui/src/main.cpp    # Compare approaches
```

**Likely useful**:
- Alternative initialization patterns
- Different RPC client structure
- May have features not in current GUI

---

## ⚠️ **Important: Code Mining First**

### **DON'T MOVE YET - Extract First**

**Step 1**: Search for unique features in each GUI

```bash
# Find unique features in x-gui
cd x-gui
find . -name "*.cpp" -exec grep -l "system tray\|notification\|QSystemTrayIcon" {} \;

# Find unique features in grandma-qt  
cd grandma-qt
find . -name "*.cpp" -exec grep -l "tutorial\|help\|wizard" {} \;
```

**Step 2**: Document useful patterns

Create `duplicates/gui-variants/REUSE_GUIDE.md`:
```markdown
# GUI Variants - Code Reuse Guide

## x-gui/ (239 files)
- System tray: `x-gui/qt/SystemTray.cpp`
- Custom widgets: `x-gui/gui-desktop/widgets/`
- Theme engine: `x-gui/qt/ThemeManager.cpp`

## grandma-qt/ (15 files)
- Big buttons: `grandma-qt/src/BigButton.cpp`
- Tutorial wizard: `grandma-qt/src/TutorialWizard.cpp`
- Simple language: `grandma-qt/src/SimplifiedText.cpp`

## dinero-qt/ (21 files)
- Alternative RPC: `dinero-qt/src/RpcClient.cpp`
- Settings dialog: `dinero-qt/src/SettingsDialog.cpp`
```

**Step 3**: Extract useful code

```bash
# Example: Extract system tray code
cp x-gui/qt/SystemTray.cpp gui/src/
cp x-gui/qt/SystemTray.h gui/include/
```

**Step 4**: THEN move to duplicates

---

## 🎯 **Final Structure**

### **Active**
```
gui/                    # Main active GUI ✅
├── src/
├── qml/
└── build/
```

### **Archived**
```
duplicates/gui-variants/
├── dinero-qt/          # Earlier Qt implementation
├── grandma-qt/         # Simplified GUI
├── x-gui/              # Experimental (239 files)
├── gui-test/           # Test data
├── test-enhanced-gui/  # Test data
├── test-gui-enhanced/  # Test data
├── test-qt-free/       # Test data
├── cli_gui/            # CLI GUI sources
├── grandma_gui/        # Grandma GUI sources
├── dinero-modern-gui.app/ # Built app
├── build-qt/           # Old build
└── REUSE_GUIDE.md      # Code mining guide
```

---

## ✅ **Verification**

After moving:

```bash
# Verify only gui/ remains
ls -d *gui* *qt* 2>/dev/null
# Should show: gui/ duplicates/ (and maybe some docs)

# Verify gui/ still works
cd gui
cmake -B build -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos"
cmake --build build -j8
```

---

## 🚀 **Execute When Ready**

**Command**:
```bash
# Run consolidation
./consolidate_guis.sh

# Or manually:
mkdir -p duplicates/gui-variants
mv dinero-qt grandma-qt x-gui gui-test test-*-gui src/cli_gui src/grandma_gui duplicates/gui-variants/
```

**Result**:
- ✅ One clear active GUI: `gui/`
- ✅ All variants preserved in `duplicates/gui-variants/`
- ✅ Can mine archived GUIs for useful code
- ✅ Cleaner project structure

---

**Status**: Ready to consolidate after code mining 🔍

