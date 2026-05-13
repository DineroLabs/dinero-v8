#!/bin/bash
# GUI Projects Consolidation Script
# Moves duplicate GUI projects to duplicates/gui-variants/

set -e

cd /Users/haydarevich/Documents/DineroCoin

echo "🧹 Dinero GUI Consolidation"
echo "==========================="
echo ""

# Create duplicates/gui-variants directory
mkdir -p duplicates/gui-variants

echo "📊 Current GUI projects:"
ls -d *gui* *qt* 2>/dev/null | grep -v "^build\|\.sh$\|\.md$\|\.log$\|^qt-" || true
echo ""

# Count files before
TOTAL_BEFORE=$(find gui dinero-qt grandma-qt x-gui gui-test test-enhanced-gui test-gui-enhanced test-qt-free src/cli_gui src/grandma_gui -type f 2>/dev/null | wc -l | tr -d ' ')
echo "📁 Total files in GUI projects: $TOTAL_BEFORE"
echo ""

read -p "❓ Proceed with consolidation? (y/n) " -n 1 -r
echo ""

if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "❌ Consolidation cancelled"
    exit 0
fi

echo ""
echo "📦 Moving Qt GUI variants to duplicates/gui-variants/..."

# Move Qt GUI variants
if [ -d "dinero-qt" ]; then
    echo "  ✓ Moving dinero-qt/ (21 files)"
    mv dinero-qt duplicates/gui-variants/
fi

if [ -d "grandma-qt" ]; then
    echo "  ✓ Moving grandma-qt/ (15 files)"
    mv grandma-qt duplicates/gui-variants/
fi

if [ -d "x-gui" ]; then
    echo "  ✓ Moving x-gui/ (239 files - largest)"
    mv x-gui duplicates/gui-variants/
fi

# Move test directories
echo ""
echo "🧪 Moving test GUI directories..."

if [ -d "gui-test" ]; then
    echo "  ✓ Moving gui-test/"
    mv gui-test duplicates/gui-variants/
fi

if [ -d "test-enhanced-gui" ]; then
    echo "  ✓ Moving test-enhanced-gui/"
    mv test-enhanced-gui duplicates/gui-variants/
fi

if [ -d "test-gui-enhanced" ]; then
    echo "  ✓ Moving test-gui-enhanced/"
    mv test-gui-enhanced duplicates/gui-variants/
fi

if [ -d "test-qt-free" ]; then
    echo "  ✓ Moving test-qt-free/"
    mv test-qt-free duplicates/gui-variants/
fi

# Move source code variants
echo ""
echo "📂 Moving src/ GUI variants..."

if [ -d "src/cli_gui" ]; then
    echo "  ✓ Moving src/cli_gui/"
    mkdir -p duplicates/gui-variants/src
    mv src/cli_gui duplicates/gui-variants/src/
fi

if [ -d "src/grandma_gui" ]; then
    echo "  ✓ Moving src/grandma_gui/"
    mkdir -p duplicates/gui-variants/src
    mv src/grandma_gui duplicates/gui-variants/src/
fi

# Move built app if exists
if [ -d "dinero-modern-gui.app" ]; then
    echo ""
    echo "📦 Moving built app bundle..."
    echo "  ✓ Moving dinero-modern-gui.app/"
    mv dinero-modern-gui.app duplicates/gui-variants/
fi

# Move build directories (but keep build-gui if it's active)
echo ""
echo "🏗️  Moving old build directories..."

if [ -d "build-qt" ]; then
    echo "  ✓ Moving build-qt/"
    mv build-qt duplicates/gui-variants/
fi

# Note: build-gui is kept if it exists (active build)

# Create reuse guide
echo ""
echo "📝 Creating code reuse guide..."

cat > duplicates/gui-variants/README.md << 'GUIDE_EOF'
# GUI Variants Archive

**Date Archived**: October 4, 2025  
**Active GUI**: `/gui/` (in project root)

## Archived GUI Projects

### **dinero-qt/** (21 files)
- Earlier Qt GUI implementation
- Last modified: Sep 28, 2025
- **Reuse**: Alternative RPC client patterns, settings dialogs

### **grandma-qt/** (15 files)
- Simplified "grandma-friendly" GUI
- Last modified: Sep 29, 2025
- **Reuse**: Big buttons, simple language, tutorial wizards
- See: `grandma-qt/README.md` for UX philosophy

### **x-gui/** (239 files - LARGEST)
- Experimental GUI with multiple variants
- Last modified: Oct 1, 2025
- **Subdirs**: gui/, gui-desktop/, embedded_gui/, qt/
- **Reuse**: Custom widgets, system tray, theme engine, desktop integration

### **Test Directories**
- `gui-test/` - Test blockchain/wallet data
- `test-enhanced-gui/` - Enhanced test configuration
- `test-gui-enhanced/` - Enhanced test configuration (variant)
- `test-qt-free/` - Qt-free test setup

### **Source Variants**
- `src/cli_gui/` - CLI-based GUI (terminal UI)
- `src/grandma_gui/` - Grandma GUI sources

### **Built Artifacts**
- `dinero-modern-gui.app/` - Built macOS app bundle
- `build-qt/` - Old Qt build directory

---

## 🔍 Code Mining Guide

### **Finding Useful Patterns**

#### **System Tray Integration**
```bash
cd x-gui
grep -r "QSystemTrayIcon\|system.*tray" . -l
```

#### **Custom Widgets**
```bash
cd x-gui
find . -name "*Widget.cpp" -o -name "*Button.cpp"
```

#### **Theme/Styling**
```bash
cd x-gui
grep -r "QStyle\|setStyleSheet\|QPalette" . -l
```

#### **Desktop Notifications**
```bash
cd x-gui
grep -r "QSystemTrayIcon.*showMessage\|Notification" . -l
```

#### **Simplified UI Patterns**
```bash
cd grandma-qt
cat src/*.cpp | grep -i "button\|dialog\|message"
```

---

## 📋 Extraction Examples

### **Extract System Tray Code**
```bash
# Find system tray implementation
find x-gui -name "*Tray*" -o -name "*tray*"

# Copy to active GUI
cp x-gui/path/to/SystemTray.* ../gui/src/
```

### **Extract Custom Widgets**
```bash
# Find custom button implementations
find x-gui grandma-qt -name "*Button.*"

# Review and extract
cp grandma-qt/src/BigButton.cpp ../gui/src/widgets/
```

### **Compare RPC Clients**
```bash
# Compare RPC implementations
diff -u dinero-qt/src/RpcClient.cpp ../gui/src/rpcclient.cpp
```

---

## ⚠️ Important Notes

### **Don't Copy Blindly**
- Review code before extracting
- Adapt to current GUI architecture
- Update includes and namespaces
- Test thoroughly

### **Why These Were Archived**
- **dinero-qt**: Superseded by `/gui/`
- **grandma-qt**: Specialized variant, not main development
- **x-gui**: Experimental, too many files
- **test dirs**: Test data only
- **src variants**: Duplicate implementations

### **Active GUI Location**
```
/Users/haydarevich/Documents/DineroCoin/gui/
```

This is the ONLY active GUI. All development happens here.

---

## 🎯 Restoration

To restore any GUI project:
```bash
# Copy back to root
cp -r duplicates/gui-variants/dinero-qt /path/to/DineroCoin/

# Or merge specific files
cp duplicates/gui-variants/x-gui/qt/SystemTray.* gui/src/
```

---

**Last Updated**: October 4, 2025  
**Status**: ✅ Archived
GUIDE_EOF

echo "  ✓ Created duplicates/gui-variants/README.md"

# Count files after
TOTAL_AFTER=$(find duplicates/gui-variants -type f 2>/dev/null | wc -l | tr -d ' ')

echo ""
echo "✅ GUI consolidation complete!"
echo "=============================="
echo ""
echo "📊 Summary:"
echo "  Files moved: $TOTAL_AFTER"
echo "  Active GUI:  gui/"
echo "  Archived:    duplicates/gui-variants/"
echo ""
echo "📁 Active GUI project:"
ls -ld gui
echo ""
echo "📁 Archived GUI projects:"
ls -l duplicates/gui-variants/ | grep "^d" | awk '{print "  -", $9}'
echo ""
echo "📖 Code reuse guide: duplicates/gui-variants/README.md"
echo ""
echo "🔍 To search archived GUIs:"
echo "  grep -r 'pattern' duplicates/gui-variants/"
echo ""

